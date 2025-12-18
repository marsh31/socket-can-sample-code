//
// 複数 ID / 複数周期を 1 スレッドで送信制御する SocketCAN サンプル
//
// - 送信スレッド: 1本
//   * timerfd によるベース周期 tick
//   * 各 ID の周期に従ってアライブカウンタ付きで送信
// - メインスレッド:
//   * アプリ本体
//   * 必要に応じて各 ID の payload を更新
//
// 注意: 各 ID の period_ms は base_period_ms の整数倍という前提。

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

// 送信管理する ID の最大数
#define MAX_TX_OBJECTS 8

// 各 ID ごとの送信オブジェクト
typedef struct {
  int enabled;     // 1: 送信有効, 0: 無効
  canid_t can_id;  // CAN ID
  uint8_t dlc;     // DLC
  int period_ms;   // この ID の周期 [ms]
  int tick_div;    // period_ms / base_period_ms

  uint8_t payload[7];     // メインスレッドから通知されるデータ [0..6]
  uint8_t alive_counter;  // アライブカウンタ
  pthread_mutex_t mtx;    // payload 保護用
} tx_object_t;

// 送信スレッド全体のコンテキスト
typedef struct {
  int sock;            // CAN ソケット
  int tfd;             // timerfd
  int base_period_ms;  // ベース周期 [ms]
  int num_objs;        // 使用中の tx_object 数
  tx_object_t objs[MAX_TX_OBJECTS];
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
  struct ifreq ifr;
  struct sockaddr_can addr;
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
  sp.sched_priority = 20;  // 環境に合わせて調整

  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
    perror("pthread_setschedparam(SCHED_FIFO)");  // 失敗しても致命的ではない
  }
}

// 送信オブジェクトを追加
//  period_ms は base_period_ms の整数倍であることを前提とする。
int tx_add_object(tx_context_t *ctx,
                  canid_t can_id,
                  uint8_t dlc,
                  int period_ms,
                  const uint8_t init_payload[7])
{
  if (ctx->num_objs >= MAX_TX_OBJECTS) {
    fprintf(stderr, "tx_add_object: too many objects\n");
    return -1;
  }

  if (period_ms <= 0 || period_ms % ctx->base_period_ms != 0) {
    fprintf(stderr,
            "tx_add_object: period_ms(%d) must be positive and "
            "a multiple of base_period_ms(%d)\n",
            period_ms,
            ctx->base_period_ms);
    return -1;
  }

  tx_object_t *obj = &ctx->objs[ctx->num_objs];
  memset(obj, 0, sizeof(*obj));

  obj->enabled       = 1;
  obj->can_id        = can_id;
  obj->dlc           = dlc;
  obj->period_ms     = period_ms;
  obj->tick_div      = period_ms / ctx->base_period_ms;
  obj->alive_counter = 0;

  pthread_mutex_init(&obj->mtx, NULL);
  memcpy(obj->payload, init_payload, 7);

  ctx->num_objs++;

  return ctx->num_objs - 1;  // index を返す
}

// メインスレッドから payload を更新する API
// index は tx_add_object が返した値
void tx_update_payload(tx_context_t *ctx, int index, const uint8_t payload[7])
{
  if (index < 0 || index >= ctx->num_objs) return;

  tx_object_t *obj = &ctx->objs[index];
  pthread_mutex_lock(&obj->mtx);
  memcpy(obj->payload, payload, 7);
  pthread_mutex_unlock(&obj->mtx);
}

// メインスレッドから ON/OFF を切り替える API（必要なら）
void tx_set_enabled(tx_context_t *ctx, int index, int enabled)
{
  if (index < 0 || index >= ctx->num_objs) return;

  ctx->objs[index].enabled = enabled ? 1 : 0;
}

// 送信スレッド本体
static void *tx_thread_main(void *arg)
{
  tx_context_t *ctx = (tx_context_t *)arg;

  set_realtime_priority_for_thread();  // 任意

  // timerfd 設定
  struct itimerspec its;
  memset(&its, 0, sizeof(its));

  int base             = ctx->base_period_ms;
  its.it_value.tv_sec  = base / 1000;
  its.it_value.tv_nsec = (base % 1000) * 1000000L;
  its.it_interval      = its.it_value;

  if (timerfd_settime(ctx->tfd, 0, &its, NULL) < 0) {
    die("timerfd_settime");
  }

  uint64_t tick = 0;

  while (!g_stop) {
    uint64_t expirations;
    ssize_t n = read(ctx->tfd, &expirations, sizeof(expirations));
    if (n < 0) {
      if (errno == EINTR) continue;
      die("read(timerfd)");
    }

    // 遅延で expirations > 1 になった場合、
    // 「遅れた分も tick を進めつつ送る」実装にしています。
    for (uint64_t e = 0; e < expirations; ++e) {
      tick++;

      for (int i = 0; i < ctx->num_objs; ++i) {
        tx_object_t *obj = &ctx->objs[i];
        if (!obj->enabled) {
          continue;
        }

        // このオブジェクトの周期に当たる tick かどうか判定
        if (obj->tick_div <= 0) continue;
        if ((tick % obj->tick_div) != 0) {
          continue;
        }

        struct canfd_frame frame;
        memset(&frame, 0, sizeof(frame));

        frame.can_id = obj->can_id;
        frame.len    = obj->dlc;

        // payload のスナップショット
        uint8_t tmp[7];
        pthread_mutex_lock(&obj->mtx);
        memcpy(tmp, obj->payload, 7);
        pthread_mutex_unlock(&obj->mtx);

        frame.data[0] = tmp[0];
        frame.data[1] = tmp[1];
        frame.data[2] = tmp[2];
        frame.data[3] = tmp[3];
        frame.data[4] = tmp[4];
        frame.data[5] = tmp[5];
        frame.data[6] = tmp[6];

        // アライブカウンタ（下位4bit利用例）
        frame.data[7]      = obj->alive_counter & 0x0F;
        obj->alive_counter = (obj->alive_counter + 1) & 0x0F;

        ssize_t sent = write(ctx->sock, &frame, sizeof(frame));
        if (sent != sizeof(frame)) {
          if (sent < 0) {
            perror("write(CAN)");
          } else {
            fprintf(stderr, "Partial CAN frame write: %zd bytes\n", sent);
          }
        }

        // 必要ならデバッグログ
        // printf("[TX] ID=0x%03X period=%dms alive=%u\n",
        //        frame.can_id, obj->period_ms, frame.data[7]);
      }
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
  ctx.sock           = sock;
  ctx.tfd            = tfd;
  ctx.base_period_ms = 10;  // ベース周期: 10ms (例)

  // === IDごとの設定例 ===
  // ID 0x100 を 10ms 周期で送信
  uint8_t payload_100[7] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  int idx_100            = tx_add_object(&ctx, 0x100, 8, 10, payload_100);

  // ID 0x200 を 50ms 周期で送信
  uint8_t payload_200[7] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
  int idx_200            = tx_add_object(&ctx, 0x200, 8, 50, payload_200);

  if (idx_100 < 0 || idx_200 < 0) {
    fprintf(stderr, "Failed to add tx objects\n");
    exit(EXIT_FAILURE);
  }

  // 送信スレッド起動
  pthread_t tx_thread;
  if (pthread_create(&tx_thread, NULL, tx_thread_main, &ctx) != 0) {
    die("pthread_create(tx_thread)");
  }

  // ===== メインスレッド：アプリ本体処理の例 =====
  printf("Main loop started. Press Ctrl+C to stop.\n");

  // サンプルとして:
  // - 100ms ごとに 0x100 の payload[0] をインクリメント
  // - 250ms ごとに 0x200 の payload[0] をインクリメント
  uint8_t p100[7];
  uint8_t p200[7];
  memcpy(p100, payload_100, 7);
  memcpy(p200, payload_200, 7);

  int cnt = 0;

  while (!g_stop) {
    usleep(50 * 1000);  // 50ms

    cnt += 50;

    if (cnt % 100 == 0) {
      p100[0]++;
      tx_update_payload(&ctx, idx_100, p100);
      // printf("[MAIN] update 0x100 payload[0]=0x%02X\n", p100[0]);
    }

    if (cnt % 250 == 0) {
      p200[0]++;
      tx_update_payload(&ctx, idx_200, p200);
      // printf("[MAIN] update 0x200 payload[0]=0x%02X\n", p200[0]);
    }

    if (cnt >= 1000) {
      cnt = 0;
    }
  }

  printf("Stopping...\n");

  // 終了処理
  pthread_join(tx_thread, NULL);

  for (int i = 0; i < ctx.num_objs; ++i) {
    pthread_mutex_destroy(&ctx.objs[i].mtx);
  }

  close(tfd);
  close(sock);

  return 0;
}
