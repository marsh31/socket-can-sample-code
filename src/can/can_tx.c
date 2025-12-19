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

        struct canfd_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.can_id = obj->can_id;
        frame.len    = obj->dlc; /* 1..64 */
        frame.flags  = obj->fd_flags;

        uint8_t snap[64];
        pthread_mutex_lock(&obj->mtx);
        memcpy(snap, obj->payload, sizeof(obj->payload));
        pthread_mutex_unlock(&obj->mtx);


        if (obj->send_cb) {
          obj->send_cb(obj->can_id, obj->dlc, frame.data, snap);
        } else {
          memcpy(frame.data, snap, obj->dlc);
        }

        ssize_t sent;
        if ((obj->fd_flags & CANFD_FDF) == CANFD_FDF) {
          sent = write(ctx->sock, &frame, sizeof(struct can_frame));
        } else {
          sent = write(ctx->sock, &frame, sizeof(struct canfd_frame));
        }
        (void)sent; /* 送信失敗はログ等にしたければここで対応 */
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
