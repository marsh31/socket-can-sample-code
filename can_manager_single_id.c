//
// メインスレッドと送信スレッドを分離した SocketCAN + timerfd サンプル
//
// - メインスレッド：アプリ処理（ここから送信データを通知）
// - 送信スレッド   ：周期タイマ + アライブカウンタ + CAN 送信
//

#define _GNU_SOURCE
#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// 送信スレッド用コンテキスト
typedef struct {
  int sock;        // CAN ソケット
  int tfd;         // timerfd
  canid_t can_id;  // 送信 CAN ID
  uint8_t dlc;     // DLC

  uint8_t payload[7];     // メインスレッドから渡される送信データ(0..6)
                          // [7] はアライブカウンタ用に送信スレッドが使用
  pthread_mutex_t mtx;    // payload 保護用
  uint8_t alive_counter;  // アライブカウンタ
  int period_ms;          // 送信周期
} tx_context_t;

static volatile sig_atomic_t g_stop = 0;

static void sigint_handler(int sig)
{
  (void)sig;
  g_stop = 1;
}

static void die(const char *msg)
{
  perror(msg);
  exit(EXIT_FAILURE);
}

// CAN RAW ソケットを canX にバインド
static int setup_can_socket(const char *ifname)
{
  int s;
  struct sockaddr_can addr;
  struct ifreq ifr;
  int enable_canfd = 1;

  s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (s < 0) {
    die("socket(PF_CAN,SOCK_RAW,CAN_RAW)");
  }

  if (setsockopt(s,
                 SOL_CAN_RAW,
                 CAN_RAW_FD_FRAMES,
                 &enable_canfd,
                 sizeof(enable_canfd))
      < 0) {
    close(s);
    die("setsocketopt CAN_RAW_FD_FRAMES is failed");
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
    die("ioctl(SIOCGIFINDEX)");
  }

  memset(&addr, 0, sizeof(addr));
  addr.can_family  = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    die("bind(canX)");
  }

  return s;
}


// 送信スレッドの優先度を少し上げる(任意)
static void set_realtime_priority_for_thread(void)
{
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = 20;  // 環境に応じて調整

  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
    perror("pthread_setschedparam(SCHED_FIFO)");  // 失敗したら警告だけ
  }
}

// メインスレッドから送信データを通知するための API
// data[7] に送信したい値を入れて呼び出す前提（長さは 7 固定とする）
void tx_update_payload(tx_context_t *ctx, const uint8_t data[7])
{
  pthread_mutex_lock(&ctx->mtx);
  memcpy(ctx->payload, data, 7);
  pthread_mutex_unlock(&ctx->mtx);
}

// 送信スレッド本体
static void *tx_thread_main(void *arg)
{
  tx_context_t *ctx = (tx_context_t *)arg;

  // 優先度アップ（任意）
  set_realtime_priority_for_thread();

  // timerfd 設定
  struct itimerspec its;
  memset(&its, 0, sizeof(its));

  int period_ms        = ctx->period_ms;
  its.it_value.tv_sec  = period_ms / 1000;
  its.it_value.tv_nsec = (period_ms % 1000) * 1000000L;
  its.it_interval      = its.it_value;

  if (timerfd_settime(ctx->tfd, 0, &its, NULL) < 0) {
    die("timerfd_settime");
  }

  while (!g_stop) {
    uint64_t expirations;
    ssize_t n = read(ctx->tfd, &expirations, sizeof(expirations));
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // シグナル割り込みなど
      }
      die("read(timerfd)");
    }

    // 遅延があると expirations > 1 になる可能性あり。
    // 「遅れた分も送る」か「最新周期1回だけ送る」かはポリシー次第。
    // ここではシンプルに "expirations 回分送る" 実装。
    for (uint64_t i = 0; i < expirations; ++i) {
      struct canfd_frame frame;
      memset(&frame, 0, sizeof(frame));

      frame.can_id = ctx->can_id;
      frame.len    = ctx->dlc;

      // 現在の payload をスナップショット
      uint8_t tmp_payload[7];
      pthread_mutex_lock(&ctx->mtx);
      memcpy(tmp_payload, ctx->payload, 7);
      pthread_mutex_unlock(&ctx->mtx);

      // data[0..6] = メインスレッドから通知されたデータ
      frame.data[0] = tmp_payload[0];
      frame.data[1] = tmp_payload[1];
      frame.data[2] = tmp_payload[2];
      frame.data[3] = tmp_payload[3];
      frame.data[4] = tmp_payload[4];
      frame.data[5] = tmp_payload[5];
      frame.data[6] = tmp_payload[6];

      // data[7] = アライブカウンタ
      frame.data[7]      = ctx->alive_counter & 0x0F;  // 下位4bit利用例
      ctx->alive_counter = (ctx->alive_counter + 1) & 0x0F;

      ssize_t sent = write(ctx->sock, &frame, sizeof(frame));
      if (sent != sizeof(frame)) {
        if (sent < 0) {
          perror("write(CAN)");
        } else {
          fprintf(stderr, "Partial CAN frame write: %zd bytes\n", sent);
        }
      }

      // デバッグ用
      // printf("[TX] ID=0x%03X alive=%u data=%02X %02X %02X %02X %02X %02X
      // %02X\n",
      //        frame.can_id, frame.data[7],
      //        frame.data[0], frame.data[1], frame.data[2],
      //        frame.data[3], frame.data[4], frame.data[5], frame.data[6]);
    }
  }

  return NULL;
}

int main(int argc, char *argv[])
{
  const char *ifname = "vcan0";
  if (argc >= 2) {
    ifname = argv[1];
  }

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  int sock = setup_can_socket(ifname);
  printf("Using interface %s\n", ifname);

  int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (tfd < 0) {
    die("timerfd_create");
  }

  tx_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.sock      = sock;
  ctx.tfd       = tfd;
  ctx.can_id    = 0x123;  // 必要に応じて変更
  ctx.dlc       = 8;
  ctx.period_ms = 10;  // 送信周期 [ms]

  pthread_mutex_init(&ctx.mtx, NULL);
  ctx.alive_counter = 0;

  // 初期 payload（メインスレッドから通知されてくるまでのデフォルト値）
  {
    uint8_t init_payload[7] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    tx_update_payload(&ctx, init_payload);
  }

  // 送信スレッド起動
  pthread_t tx_thread;
  if (pthread_create(&tx_thread, NULL, tx_thread_main, &ctx) != 0) {
    die("pthread_create(tx_thread)");
  }

  // ===== メインスレッド側：アプリ本体の処理 =====
  // ここではサンプルとして、一定周期で payload を書き換えるだけにしています。
  // 実際には、センサ入力や他プロセスからのコマンドなどから値を更新する想定。
  printf("Main thread: start application loop. Press Ctrl+C to stop.\n");

  uint8_t app_payload[7] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

  while (!g_stop) {
    // 例: 100ms ごとに適当に payload[0] を更新
    usleep(100 * 1000);

    app_payload[0]++;  // ここでは単純にインクリメントするだけの例

    // 送信スレッドへ「新しい送信データ」を通知
    tx_update_payload(&ctx, app_payload);

    // デバッグ表示(必要なら)
    printf("[MAIN] update payload: %02X %02X %02X %02X %02X %02X %02X\n",
           app_payload[0],
           app_payload[1],
           app_payload[2],
           app_payload[3],
           app_payload[4],
           app_payload[5],
           app_payload[6]);
  }

  printf("Stopping...\n");

  // 終了処理
  pthread_join(tx_thread, NULL);
  pthread_mutex_destroy(&ctx.mtx);
  close(tfd);
  close(sock);

  return 0;
}
