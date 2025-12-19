// 受信制御モジュール
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_internal.h"

static void *rx_thread_main(void *arg)
{
  can_context_t *ctx = (can_context_t *)arg;
  while (ctx->running) {
    struct pollfd pfd;
    pfd.fd = ctx->sock;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 200); /* 200ms 周期で終了条件を確認 */
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue; /* timeout */
    if (!(pfd.revents & POLLIN)) continue;

    unsigned char buf[sizeof(struct canfd_frame)] = {0};
    ssize_t n = read(ctx->sock, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      break; /* その他のエラーは終了 */
    }

    struct canfd_frame cfd;
    if ((size_t)n == sizeof(struct canfd_frame)) {
      memcpy(&cfd, buf, sizeof(struct canfd_frame));
    } else if ((size_t)n == sizeof(struct can_frame)) {
      struct can_frame *cf = (struct can_frame *)buf;
      memset(&cfd, 0, sizeof(cfd));
      cfd.can_id = cf->can_id;
      cfd.len    = cf->can_dlc;
      cfd.flags  = 0;
      memcpy(cfd.data, cf->data, (size_t)cfd.len);
    } else {
      /* サイズ不整合 */
      continue;
    }

    can_rx_callback_t cb = ctx->rx_cb;
    if (cb) cb(&cfd, ctx->rx_user);
  }
  return NULL;
}

int can_rx_start(can_context_t *ctx)
{
  if (!ctx) return -1;
  if (ctx->rx_started) return 0;
  if (pthread_create(&ctx->rx_thread, NULL, rx_thread_main, ctx) == 0) {
    ctx->rx_started = 1;
    return 0;
  }
  return -1;
}

void can_rx_stop(can_context_t *ctx)
{
  if (!ctx || !ctx->rx_started) return;
  pthread_join(ctx->rx_thread, NULL);
  ctx->rx_started = 0;
}
