/* FILE:     can_internal.h
 * AUTHOR:   marsh
 * NOTE:
 * 内部用ヘッダで公開APIではありません。
 * 外部モジュールから参照はしないでください。
 */

#ifndef CAN_INTERNAL_H
#define CAN_INTERNAL_H

#include <linux/can.h>
#include <pthread.h>
#include <stdint.h>

#include "can_manager.h" /* public API types (can_rx_callback_t) */

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TX_OBJECTS 8

typedef struct tx_object_t {
  int enabled;

  canid_t can_id;
  uint8_t dlc;
  uint8_t fd_flags; /* CAN FD flags */

  int period_ms;
  int tick_div;

  pthread_mutex_t mtx;
  uint8_t payload[64];

  can_tx_update_cb_t update_cb;
  can_tx_send_cb_t send_cb;

} tx_object_t;

typedef struct can_context_t {
  int sock;           /* CAN ソケット */
  int tfd;            /* timerfd */
  int base_period_ms; /* ベース周期[ms] */

  int running; /* スレッド稼働中フラグ */

  pthread_t tx_thread; /* 送信スレッド */
  pthread_t rx_thread; /* 受信スレッド */

  int tx_started; /* 送信スレッド起動済み */
  int rx_started; /* 受信スレッド起動済み */

  /* 受信通知 */
  can_rx_callback_t rx_cb;
  void *rx_user;

  struct tx_object_t objs[MAX_TX_OBJECTS];
  int num_objs; /* 使用中オブジェクト数 */
} can_context_t;

/* モジュール境界の関数 */
int can_tx_start(can_context_t *ctx);
void can_tx_stop(can_context_t *ctx);

int can_rx_start(can_context_t *ctx);
void can_rx_stop(can_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAN_INTERNAL_H */
