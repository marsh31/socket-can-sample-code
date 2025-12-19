// CAN FD 送受信ライブラリの使用サンプル
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "can_manager.h"

static volatile sig_atomic_t g_stop = 0;
static void sigint_handler(int sig)
{
  (void)sig;
  g_stop = 1;
}

/* 受信コールバック: 受信したフレーム情報を表示する */
static void on_rx(const struct canfd_frame *f, void *user)
{
  (void)user;
  if (!f) return;
  fprintf(stdout,
          "RX id=0x%03X len=%u flags=0x%02X data=",
          (unsigned)f->can_id,
          (unsigned)f->len,
          (unsigned)f->flags);
  unsigned int n = f->len;
  if (n > 64) n = 64;
  for (unsigned int i = 0; i < n; ++i) fprintf(stdout, "%02X ", f->data[i]);
  fputc('\n', stdout);
}

int main(int argc, char *argv[])
{
  const char *ifname = (argc >= 2) ? argv[1] : "vcan0";

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  /* ベース周期 10ms で管理 */
  can_context_t *ctx = open_canfd(ifname, 10);
  if (!ctx) {
    fprintf(stderr, "open_canfd failed for %s\n", ifname);
    return 1;
  }

  /* 受信通知を登録（不要なら NULL を指定）*/
  set_canfd_rx_callback(ctx, on_rx, NULL);

  /* 例1: DLC=8 (従来CAN)、10ms 周期 */
  uint8_t p100[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  int idx100 = add_canfd_frame(ctx, 0x100, 8, 10, p100, 1, 0, 1, NULL, NULL);

  /* 例2: CAN FD DLC=32、50ms 周期、BRS 有効 */
  uint8_t p200[32];
  for (int i = 0; i < 32; ++i) p200[i] = (uint8_t)(i);
  int idx200 = add_canfd_frame(
      ctx, can_extended_format(0x11000000), 32, 50, p200, 1, 0, 1, NULL, NULL);

  if (idx100 < 0 || idx200 < 0) {
    fprintf(stderr, "add_canfd_frame failed\n");
    close_canfd(ctx);
    return 1;
  }

  if (start_canfd(ctx) != 0) {
    fprintf(stderr, "start_canfd failed\n");
    close_canfd(ctx);
    return 1;
  }

  printf("Sending on %s. Press Ctrl+C to stop.\n", ifname);

  int elapsed_ms = 0;
  while (!g_stop) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50L * 1000L * 1000L};
    nanosleep(&ts, NULL);
    elapsed_ms += 50;

    if (elapsed_ms % 100 == 0) {
      p100[0]++;
      /* ID指定の更新（互換API）*/
      tx_update_payload(ctx, 0x100, p100, 8);
    }

    if (elapsed_ms % 250 == 0) {
      /* 先頭だけ更新（例） */
      p200[0]++;
      tx_update_payload(ctx, can_extended_format(0x11000000), p200, 32);
    }

    if (elapsed_ms >= 10 * 1000) { /* 10秒で自動終了(必要なら無限運転) */
      break;
    }
  }

  printf("Stopping...\n");
  stop_canfd(ctx);
  close_canfd(ctx);
  return 0;
}
