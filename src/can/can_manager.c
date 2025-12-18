// ライブラリ: 複数ID/複数周期を1スレッドで送信管理

#define _GNU_SOURCE
#include "can_manager.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
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

/* ビットレート切替(BRS)を使う場合は1にする */
#ifndef CANMGR_ENABLE_BRS
#define CANMGR_ENABLE_BRS 0
#endif

struct tx_object_t {
  int enabled;           /* 1:送信有効 */
  canid_t can_id;        /* CAN ID */
  uint8_t dlc;           /* DLC (8以上推奨) */
  int period_ms;         /* 送信周期[ms] */
  int tick_div;          /* period_ms / base_period_ms */

  uint8_t payload[64];   /* アプリ提供データ(最大64B)。APIでは先頭7Bを使用 */
  uint8_t alive_counter; /* 下位4bitで使用する例 */
  uint8_t fd_flags;      /* CAN FD flags (e.g. CANFD_BRS) */
  pthread_mutex_t mtx;   /* payload 保護 */
};

struct tx_context_t {
  int sock;              /* CAN ソケット */
  int tfd;               /* timerfd */
  int base_period_ms;    /* ベース周期[ms] */

  int running;           /* スレッド稼働中フラグ */
  pthread_t tx_thread;   /* 送信スレッド */

  int num_objs;                      /* 使用中オブジェクト数 */
  struct tx_object_t objs[MAX_TX_OBJECTS];
};

static int  setup_can_socket(const char *ifname);
static int  setup_timerfd(int base_period_ms);
static void set_realtime_priority_for_thread(void);
static void *tx_thread_main(void *arg);

tx_context_t *open_canfd(const char *ifname, int base_period_ms)
{
  tx_context_t *ctx = (tx_context_t *)calloc(1, sizeof(tx_context_t));
  if (!ctx) return NULL;

  ctx->sock = setup_can_socket(ifname);
  if (ctx->sock < 0) {
    free(ctx);
    return NULL;
  }

  ctx->base_period_ms = base_period_ms;
  ctx->tfd            = setup_timerfd(base_period_ms);
  if (ctx->tfd < 0) {
    close(ctx->sock);
    free(ctx);
    return NULL;
  }

  ctx->running  = 0;
  ctx->num_objs = 0;
  return ctx;
}

void close_canfd(tx_context_t *ctx)
{
  if (!ctx) return;
  if (ctx->running) {
    stop_canfd(ctx);
  }
  for (int i = 0; i < ctx->num_objs; ++i) {
    pthread_mutex_destroy(&ctx->objs[i].mtx);
  }
  if (ctx->tfd >= 0) close(ctx->tfd);
  if (ctx->sock >= 0) close(ctx->sock);
  free(ctx);
}

int start_canfd(tx_context_t *ctx)
{
  if (!ctx) return -1;
  if (ctx->running) return 0;
  ctx->running = 1;
  if (pthread_create(&ctx->tx_thread, NULL, tx_thread_main, ctx) != 0) {
    ctx->running = 0;
    return -1;
  }
  return 0;
}

void stop_canfd(tx_context_t *ctx)
{
  if (!ctx || !ctx->running) return;
  ctx->running = 0;
  /* できるだけ早く read() を抜けさせるため、最短周期に更新 */
  if (ctx->tfd >= 0) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec    = 1; /* ほぼ即時 */
    its.it_interval.tv_nsec = 1;
    timerfd_settime(ctx->tfd, 0, &its, NULL);
  }
  pthread_join(ctx->tx_thread, NULL);
}

int add_canfd_frame(tx_context_t *ctx,
                    canid_t can_id,
                    uint8_t dlc,
                    int period_ms,
                    const uint8_t init_payload[7])
{
  if (!ctx) return -1;
  if (ctx->num_objs >= MAX_TX_OBJECTS) return -1;
  if (period_ms <= 0 || period_ms % ctx->base_period_ms != 0) return -1;

  tx_object_t *obj = &ctx->objs[ctx->num_objs];
  memset(obj, 0, sizeof(*obj));
  obj->enabled     = 1;
  obj->can_id      = can_id;
  /* CAN FD: len(=dlc)は1..64（末尾1Bはアライブ）*/
  obj->dlc         = (dlc < 1) ? 1 : (dlc > 64 ? 64 : dlc);
  obj->period_ms   = period_ms;
  obj->tick_div    = period_ms / ctx->base_period_ms;
  obj->fd_flags    = 0;
  pthread_mutex_init(&obj->mtx, NULL);
  memset(obj->payload, 0, sizeof(obj->payload));
  memcpy(obj->payload, init_payload, 7);
  obj->alive_counter = 0;

  ctx->num_objs++;
  return ctx->num_objs - 1; /* index */
}

int add_canfd_frame_ex(tx_context_t *ctx,
                       canid_t can_id,
                       uint8_t len,
                       int period_ms,
                       const uint8_t *payload,
                       int use_brs)
{
  if (!ctx) return -1;
  if (ctx->num_objs >= MAX_TX_OBJECTS) return -1;
  if (period_ms <= 0 || period_ms % ctx->base_period_ms != 0) return -1;
  if (len < 1) return -1;              /* alive を置くため最低1 */
  if (len > 64) len = 64;

  tx_object_t *obj = &ctx->objs[ctx->num_objs];
  memset(obj, 0, sizeof(*obj));
  obj->enabled     = 1;
  obj->can_id      = can_id;
  obj->dlc         = len;
  obj->period_ms   = period_ms;
  obj->tick_div    = period_ms / ctx->base_period_ms;
  obj->fd_flags    = use_brs ? CANFD_BRS : 0;
  pthread_mutex_init(&obj->mtx, NULL);
  memset(obj->payload, 0, sizeof(obj->payload));
  if (payload) {
    size_t copy = (len - 1) > 63 ? 63 : (size_t)(len - 1);
    memcpy(obj->payload, payload, copy);
  }
  obj->alive_counter = 0;

  ctx->num_objs++;
  return ctx->num_objs - 1;
}

void tx_update_payload(tx_context_t *ctx, int index, const uint8_t payload[7])
{
  if (!ctx) return;
  if (index < 0 || index >= ctx->num_objs) return;
  tx_object_t *obj = &ctx->objs[index];
  pthread_mutex_lock(&obj->mtx);
  memcpy(obj->payload, payload, 7);
  pthread_mutex_unlock(&obj->mtx);
}

void tx_update_payload_ex(tx_context_t *ctx, int index, const uint8_t *payload, uint8_t len)
{
  if (!ctx || !payload) return;
  if (index < 0 || index >= ctx->num_objs) return;
  tx_object_t *obj = &ctx->objs[index];
  uint8_t max_copy = (obj->dlc > 0) ? (obj->dlc - 1) : 0;
  if (len > max_copy) len = max_copy;
  pthread_mutex_lock(&obj->mtx);
  /* 先頭から len バイトだけ更新（残りは保持）*/
  if (len > 0) memcpy(obj->payload, payload, len);
  pthread_mutex_unlock(&obj->mtx);
}

void tx_set_enabled(tx_context_t *ctx, int index, int enabled)
{
  if (!ctx) return;
  if (index < 0 || index >= ctx->num_objs) return;
  ctx->objs[index].enabled = enabled ? 1 : 0;
}

void tx_set_brs(tx_context_t *ctx, int index, int enable)
{
  if (!ctx) return;
  if (index < 0 || index >= ctx->num_objs) return;
  if (enable)
    ctx->objs[index].fd_flags |= CANFD_BRS;
  else
    ctx->objs[index].fd_flags &= (uint8_t)~CANFD_BRS;
}

static void set_realtime_priority_for_thread(void)
{
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = 20; /* 環境に応じて調整 */
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); /* 失敗しても致命的ではない */
}

static void *tx_thread_main(void *arg)
{
  tx_context_t *ctx = (tx_context_t *)arg;
  uint64_t tick = 0;
  set_realtime_priority_for_thread();

  while (ctx->running) {
    uint64_t expirations = 0;
    ssize_t n = read(ctx->tfd, &expirations, sizeof(expirations));
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

        /* アプリデータは dlc-1 バイトまで、末尾1B をアライブに使用 */
        uint8_t snap[64];
        pthread_mutex_lock(&obj->mtx);
        memcpy(snap, obj->payload, sizeof(obj->payload));
        pthread_mutex_unlock(&obj->mtx);

        int len = frame.len <= 0 ? 1 : frame.len;
        int alive_idx = len - 1;
        int app_len   = alive_idx;
        if (app_len > 0) memcpy(frame.data, snap, (size_t)app_len);
        frame.data[alive_idx] = obj->alive_counter & 0x0F; /* 下位4bit */
        obj->alive_counter = (obj->alive_counter + 1) & 0x0F;

        ssize_t sent = write(ctx->sock, &frame, sizeof(frame));
        if (sent != sizeof(frame)) {
          /* 送信失敗はログのみ */
          if (sent < 0) perror("write(CAN)");
        }
      }
    }
  }
  return NULL;
}

static int setup_can_socket(const char *ifname)
{
  int s;
  struct ifreq ifr;
  struct sockaddr_can addr;
  int enable_canfd = 1;

  s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (s < 0) return -1;

  if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable_canfd, sizeof(enable_canfd)) < 0) {
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
  int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (tfd < 0) return -1;

  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_sec  = base_period_ms / 1000;
  its.it_value.tv_nsec = (base_period_ms % 1000) * 1000000L;
  its.it_interval      = its.it_value; /* 周期 */
  if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
    close(tfd);
    return -1;
  }
  return tfd;
}
