// ライブラリ: 送受信マネージャ（スレッド制御／ソケット初期化）

#define _GNU_SOURCE
#include "can_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
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

#include "can_internal.h"

/* 前方宣言 */
static int setup_can_socket(const char *ifname);
static int setup_timerfd(int base_period_ms);
static int search_cantx_index(can_context_t *ctx, canid_t can_id);

can_context_t *open_canfd(const char *ifname, int base_period_ms)
{
  can_context_t *ctx = (can_context_t *)calloc(1, sizeof(can_context_t));
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

  ctx->running    = 0;
  ctx->tx_started = 0;
  ctx->rx_started = 0;
  ctx->rx_cb      = NULL;
  ctx->rx_user    = NULL;
  ctx->num_objs   = 0;
  return ctx;
}

void close_canfd(can_context_t *ctx)
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

int start_canfd(can_context_t *ctx)
{
  if (!ctx) return -1;
  if (ctx->running) return 0;

  ctx->running = 1;
  if (can_tx_start(ctx) != 0) {
    ctx->running = 0;
    return -1;
  }

  /* 受信スレッドは失敗しても送信のみで継続 */
  (void)can_rx_start(ctx);

  return 0;
}

void stop_canfd(can_context_t *ctx)
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

  can_tx_stop(ctx);
  can_rx_stop(ctx);
}

canid_t can_extended_format(canid_t can_id)
{
  return (can_id | CAN_EFF_FLAG);
}


int add_canfd_frame(can_context_t *ctx,
                    canid_t can_id,
                    uint8_t len, /* 1..64 (末尾1Bはalive) */
                    int period_ms,
                    const uint8_t *payload, /* payload_len = len-1 */
                    int use_brs,            /* 1:CANFD_BRS */
                    int use_esi,
                    int use_fdf,
                    can_tx_update_cb_t update_cb,
                    can_tx_send_cb_t send_cb)
{
  if (!ctx) return -1;
  if (ctx->num_objs >= MAX_TX_OBJECTS) return -1;
  if (period_ms <= 0 || period_ms % ctx->base_period_ms != 0) return -1;
  if (len < 0) return -1;
  if (len > 64) len = 64;

  tx_object_t *obj = &ctx->objs[ctx->num_objs];
  memset(obj, 0, sizeof(*obj));

  obj->enabled   = 1;
  obj->can_id    = can_id;
  obj->dlc       = len;
  obj->period_ms = period_ms;
  obj->tick_div  = period_ms / ctx->base_period_ms;

  int brs_flags = use_brs ? CANFD_BRS : 0;
  int esi_flags = use_esi ? CANFD_ESI : 0;
  int fdf_flags = use_fdf ? CANFD_FDF : 0;
  obj->fd_flags = brs_flags | esi_flags | fdf_flags;

  pthread_mutex_init(&obj->mtx, NULL);
  memset(obj->payload, 0, sizeof(obj->payload));
  if (payload) {
    memcpy(obj->payload, payload, len);
  }

  ctx->num_objs++;
  return ctx->num_objs - 1;
}


void tx_update_payload(can_context_t *ctx,
                       canid_t can_id,
                       const uint8_t *payload,
                       uint8_t len)
{
  if (!ctx || !payload) return;

  int index = search_cantx_index(ctx, can_id);
  if (index < 0 || index >= ctx->num_objs) return;

  tx_object_t *obj = &ctx->objs[index];

  pthread_mutex_lock(&obj->mtx);

  if (obj->update_cb) {
    obj->update_cb(obj->can_id, obj->dlc, obj->payload, payload);
  } else {
    memcpy(obj->payload, payload, obj->dlc);
  }

  pthread_mutex_unlock(&obj->mtx);
}

void tx_set_enabled(can_context_t *ctx, int index, int enabled)
{
  if (!ctx) return;
  if (index < 0 || index >= ctx->num_objs) return;
  ctx->objs[index].enabled = enabled ? 1 : 0;
}

void tx_set_brs(can_context_t *ctx, int index, int enable)
{
  if (!ctx) return;
  if (index < 0 || index >= ctx->num_objs) return;
  if (enable)
    ctx->objs[index].fd_flags |= CANFD_BRS;
  else
    ctx->objs[index].fd_flags &= (uint8_t)~CANFD_BRS;
}

static int setup_can_socket(const char *ifname)
{
  int s;
  struct ifreq ifr;
  struct sockaddr_can addr;
  int enable_canfd = 1;

  s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (s < 0) return -1;

  if (setsockopt(s,
                 SOL_CAN_RAW,
                 CAN_RAW_FD_FRAMES,
                 &enable_canfd,
                 sizeof(enable_canfd))
      < 0) {
    close(s);
    return -1;
  }

  /* poll() を使うため非ブロッキング化（write の EAGAIN は上位で許容）*/
  int fl = fcntl(s, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);

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

static int search_cantx_index(can_context_t *ctx, canid_t can_id)
{
  if (!ctx) return -1;
  for (int i = 0; i < ctx->num_objs; i++) {
    if (ctx->objs[i].can_id == can_id) {
      return i;
    }
  }

  return -1;
}

void set_canfd_rx_callback(can_context_t *ctx, can_rx_callback_t cb, void *user)
{
  if (!ctx) return;
  ctx->rx_cb   = cb;
  ctx->rx_user = user;
}
