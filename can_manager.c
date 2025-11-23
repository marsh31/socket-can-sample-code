
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

#define MAX_TX_OBJECTS 8

// 送信スレッド全体のコンテキスト
struct tx_context_t {
  int sock;            // CAN ソケット
  int tfd;             // timerfd
  int base_period_ms;  // ベース周期 [ms]


  int is_running;       // 送信スレッドが動いているか？
  pthread_t tx_thread;  // 送信スレッド

  int num_objs;                      // 使用中の tx_object 数
  tx_object_t objs[MAX_TX_OBJECTS];  // 使用中の tx_object
};


struct tx_object_t {
  int enabled;
  canid_t can_id;
  uint8_t dlc;
  int period_ms;
  int tick_div;

  uint8_t payload[64];
  pthread_mutex_t mtx;
};


static int setup_can_socket(const char *ifname);
static int setup_timerfd();
static void *tx_thread_main(void *arg);

tx_context_t *open_canfd(const char *ifname, int base_period_ms)
{
  int timerfd;

  tx_context_t *ctx = (tx_context_t *)malloc(sizeof(tx_context_t));

  ctx->sock = setup_can_socket(ifname);
  if (ctx->sock < 0) {
    free(ctx);
    return NULL;
  }

  ctx->base_period_ms = base_period_ms;
  ctx->tfd            = setup_timerfd(base_period_ms);
  if (ctx->tfd < 0) {
    free(ctx);
    return NULL;
  }

  ctx->num_objs = 0;

  return ctx;
}


void close_canfd(tx_context_t *ctx)
{
  close(ctx->sock);
  close(ctx->tfd);
  free(ctx);
}


int start_canfd(tx_context_t *ctx)
{
  return pthread_create(&ctx->tx_thread, NULL, tx_thread_main, &ctx) != 0);
}


void stop_canfd(tx_context_t *ctx)
{
  // TODO: update while loop status in tx_thread_main
  pthread_join(tx_thread, NULL);
}


int add_canfd_frame(tx_context_t *ctx,
                    canid_t can_id,
                    uint8_t dlc,
                    int period_ms,
                    const uint8_t init_payload)
{
  if (ctx->num_objs >= MAX_TX_OBJECTS) {
    return -1;
  }

  if (period_ms <= 0 || period_ms % ctx->base_period_ms != 0) {
    return -1;
  }

  tx_object_t *obj = &ctx->objs[ctx->num_objs];
  memset(obj, 0, sizeof(*obj));

  obj->enabled   = 1;
  obj->can_id    = can_id;
  obj->dlc       = dlc;
  obj->period_ms = period_ms;
  obj->tick_div  = period_ms / ctx->base_period_ms;

  pthread_mutex_init(&obj->mtx, NULL);
  memcpy(obj->payload, init_payload, obj->dlc);
  ctx->num_objs++;

  return ctx->num_objs - 1;
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
    return -1;
  }

  if (setsockopt(s,
                 SOL_CAN_RAW,
                 CAN_RAW_FD_FRAMES,
                 &enable_canfd,
                 sizeof(enable_canfd))
      < 0) {
    close(s);
    return -1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
    close(s);
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.can_family  = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(s);
    return -1;
  }

  return s;
}


static int setup_timerfd(int base_period_ms)
{
  int tfd;
  struct itimerspec its;

  tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (tfd < 0) {
    return -1;
  }

  memset(&its, 0, sizeof(its));
  its.it_value.tv_sec  = base_period_ms / 1000;
  its.it_value.tv_nsec = (base_period_ms % 1000) * 1000000L;
  its.it_interval      = its.it_value;

  if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
    return -1;
  }

  return tfd;
}

static void *tx_thread_main(void *arg)
{
  uint64_t tick     = 0;
  tx_context_t *ctx = (tx_context_t *)arg;

  while (1) {
    ssize_t n = read(ctx->tfd, &expirations, sizeof(expirations));

    if (n < 0) {
      if (errno == EINTR) continue;
      return NULL;
    }

    // 遅延で expirations > 1 になった場合、
    // 「遅れた分も tick を進めつつ送る」実装。
    for (uint64_t e = 0; e < expirations; e++) {
      tick++;

      for (int i = 0; i < ctx->num_objs; i++) {
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
