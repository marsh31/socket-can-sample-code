// 送信制御モジュール
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "can_internal.h"

static void set_realtime_priority_for_thread(void)
{
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = 20; /* 環境に応じて調整可 */
  pthread_setschedparam(
      pthread_self(), SCHED_FIFO, &sp); /* 失敗しても致命的ではない */
}

static void *tx_thread_main(void *arg)
{
  can_context_t *ctx = (can_context_t *)arg;
  uint64_t tick      = 0;
  set_realtime_priority_for_thread();

  while (ctx->running) {
    uint64_t expirations = 0;
    ssize_t n            = read(ctx->tfd, &expirations, sizeof(expirations));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (uint64_t e = 0; e < expirations; ++e) {
      tick++;
      for (int i = 0; i < ctx->num_objs; ++i) {
        tx_object_t *obj = &ctx->objs[i];
        if (!obj->enabled) continue;
        if (obj->tick_div <= 0) continue;
        if ((tick % obj->tick_div) != 0) continue;

        /* payloadのスナップショットを先に取得 */
        uint8_t snap[64];
        pthread_mutex_lock(&obj->mtx);
        memcpy(snap, obj->payload, sizeof(obj->payload));
        pthread_mutex_unlock(&obj->mtx);

        /* CAN/CAN FD いずれにも対応するため共用体で生成 */
        union {
          struct can_frame cf;
          struct canfd_frame cfd;
        } u;
        memset(&u, 0, sizeof(u));

        const int is_fd = (obj->fd_flags & CANFD_FDF) == CANFD_FDF;
        if (is_fd) {
          u.cfd.can_id = obj->can_id;
          u.cfd.len    = obj->dlc;
          u.cfd.flags  = obj->fd_flags;
          if (obj->send_cb) {
            obj->send_cb(obj->can_id, obj->dlc, u.cfd.data, snap);
          } else {
            memcpy(u.cfd.data, snap, obj->dlc);
          }
          (void)write(ctx->sock, &u.cfd, sizeof(struct canfd_frame));
        } else {
          u.cf.can_id  = obj->can_id;
          u.cf.can_dlc = (obj->dlc > 8) ? 8 : obj->dlc;
          if (obj->send_cb) {
            obj->send_cb(obj->can_id, u.cf.can_dlc, u.cf.data, snap);
          } else {
            memcpy(u.cf.data, snap, u.cf.can_dlc);
          }
          (void)write(ctx->sock, &u.cf, sizeof(struct can_frame));
        }
        /* 送信失敗は必要に応じてログ化を検討 */
      }
    }
  }
  return NULL;
}

int can_tx_start(can_context_t *ctx)
{
  if (!ctx) return -1;
  if (ctx->tx_started) return 0;
  if (pthread_create(&ctx->tx_thread, NULL, tx_thread_main, ctx) == 0) {
    ctx->tx_started = 1;
    return 0;
  }
  return -1;
}

void can_tx_stop(can_context_t *ctx)
{
  if (!ctx || !ctx->tx_started) return;
  pthread_join(ctx->tx_thread, NULL);
  ctx->tx_started = 0;
}
