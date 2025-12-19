/*
 * CAN マルチ ID 送受信マネージャ ライブラリ (CAN FD対応)
 */

#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <linux/can.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct can_context_t can_context_t;
typedef struct tx_object_t tx_object_t;

typedef struct can_rx_queue_t can_rx_queue_t;

typedef void (*can_tx_update_cb_t)(canid_t can_id,
                                   uint8_t dlc,
                                   uint8_t *payload,
                                   const uint8_t *update_value);
typedef void (*can_tx_send_cb_t)(canid_t can_id,
                                 uint8_t dlc,
                                 uint8_t *payload,
                                 const uint8_t *update_value);

/* 受信コールバック型
 * 注意: 引数 frame はコールバック呼び出し中のみ有効（参照は都度コピー）。
 */
typedef void (*can_rx_callback_t)(const struct canfd_frame *frame, void *user);

/* コンテキスト生成/破棄 */
can_context_t *open_canfd(const char *ifname, int base_period_ms);
void close_canfd(can_context_t *ctx);

/* 送受信スレッド制御 */
int start_canfd(can_context_t *ctx); /* 送受信スレッド開始: 0:OK, -1:NG */
void stop_canfd(can_context_t *ctx); /* 送受信スレッド停止（join まで）*/

/* 受信コールバック設定（NULL で解除）*/
void set_canfd_rx_callback(can_context_t *ctx,
                           can_rx_callback_t cb,
                           void *user);

/* CAN IDを拡張フォーマットに変換する */
canid_t can_extended_format(canid_t can_id);

/* can id
 *   standard: 0x000 ~ 0x7FF
 *   extended: 0x00000000 ~ 0x1FFFFFFF
 */
/* 送信オブジェクト登録（拡張: 可変長/FD BRS）*/
int add_canfd_frame(can_context_t *ctx,
                    canid_t can_id,
                    uint8_t len, /* 1..64 */
                    int period_ms,
                    const uint8_t *payload, /* payload_len = len */
                    int use_brs,            /* 1:CANFD_BRS */
                    int use_esi,
                    int use_fdf, /* 1:FDフレーム */
                    can_tx_update_cb_t update_cb,
                    can_tx_send_cb_t send_cb);

/* ペイロード更新
 * 注意: 既存API(tx_update_payload)はID指定でしたが可読性と安全性のため
 * by-index 版を追加しました。ID版は下位互換のため残します。*/
void tx_update_payload_by_id(can_context_t *ctx,
                             canid_t can_id,
                             const uint8_t *payload,
                             uint8_t len);

void tx_update_payload_by_index(can_context_t *ctx,
                                int index,
                                const uint8_t *payload,
                                uint8_t len);

/* 非推奨: 互換のため残置（ID指定）。新規コードは _by_id / _by_index を使用 */
void tx_update_payload(can_context_t *ctx,
                       canid_t can_id,
                       const uint8_t *payload,
                       uint8_t len);

/* 有効/無効やBRS切替 */
void tx_set_enabled(can_context_t *ctx, int index, int enabled);
void tx_set_brs(can_context_t *ctx, int index, int enable);


can_rx_queue_t *can_rx_queue_init(void);
void can_rx_queue_destroy(can_rx_queue_t *obj);

int can_rx_queue_push(can_rx_queue_t *q, const struct canfd_frame *frame);
int can_rx_queue_pop(can_rx_queue_t *q, struct canfd_frame *frame);

int can_rx_queue_try_push(can_rx_queue_t *q, const struct canfd_frame *frame);
int can_rx_queue_try_pop(can_rx_queue_t *q, struct canfd_frame *frame);

#ifdef __cplusplus
}
#endif

#endif /* CAN_MANAGER_H */
