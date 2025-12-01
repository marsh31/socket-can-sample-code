// CAN FD送信ライブラリの使用サンプル
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "can_manager.h"

static volatile sig_atomic_t g_stop = 0;
static void sigint_handler(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char *argv[])
{
  const char *ifname = (argc >= 2) ? argv[1] : "vcan0";

  signal(SIGINT,  sigint_handler);
  signal(SIGTERM, sigint_handler);

  /* ベース周期 10ms で管理 */
  tx_context_t *ctx = open_canfd(ifname, 10);
  if (!ctx) {
    fprintf(stderr, "open_canfd failed for %s\n", ifname);
    return 1;
  }

  /* 例1: DLC=8 (従来互換)、10ms 周期 */
  uint8_t p100[7] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77};
  int idx100 = add_canfd_frame(ctx, 0x100, 8, 10, p100);

  /* 例2: CAN FD DLC=32、50ms 周期、BRS 有効 */
  uint8_t p200[31];
  for (int i = 0; i < 31; ++i) p200[i] = (uint8_t)(i);
  int idx200 = add_canfd_frame_ex(ctx, 0x200, 32, 50, p200, 1);

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
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50L * 1000L * 1000L };
    nanosleep(&ts, NULL);
    elapsed_ms += 50;

    if (elapsed_ms % 100 == 0) {
      p100[0]++;
      tx_update_payload(ctx, idx100, p100);
    }

    if (elapsed_ms % 250 == 0) {
      /* 31バイトの先頭だけ更新 */
      p200[0]++;
      tx_update_payload_ex(ctx, idx200, p200, 31);
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
