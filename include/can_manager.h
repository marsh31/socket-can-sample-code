/*
 * CAN マルチ ID 送受信マネージャ ライブラリ (CAN FD対応)
 * - 送信: timerfd ベースの1スレッドで複数 ID/周期を送信
 * - 受信: 別スレッドでソケットから受信し、コールバックへ通知
 * - data[0..(len-2)] はアプリ側、data[(len-1)] はアライブカウンタ
 */

#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <stdint.h>
#include <linux/can.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct tx_context_t tx_context_t;
typedef struct tx_object_t  tx_object_t;

/* 受信コールバック型
 * 注意: 引数 frame はコールバック呼び出し中のみ有効（参照は都度コピー）。
 */
typedef void (*can_rx_callback_t)(const struct canfd_frame *frame, void *user);

/* コンテキスト生成/破棄 */
tx_context_t *open_canfd(const char *ifname, int base_period_ms);
void          close_canfd(tx_context_t *ctx);

/* 送受信スレッド制御 */
int  start_canfd(tx_context_t *ctx);  /* 送受信スレッド開始: 0:OK, -1:NG */
void stop_canfd(tx_context_t *ctx);   /* 送受信スレッド停止（join まで）*/

/* 受信コールバック設定（NULL で解除）*/
void set_canfd_rx_callback(tx_context_t *ctx, can_rx_callback_t cb, void *user);

/* 送信オブジェクト登録（簡易: DLC>=8, payloadは先頭7B）*/
int  add_canfd_frame(tx_context_t *ctx,
                     canid_t can_id,
                     uint8_t dlc,
                     int period_ms,
                     const uint8_t init_payload[7]); /* 戻り値: index/-1 */

/* 送信オブジェクト登録（拡張: 可変長/FD BRS）*/
int  add_canfd_frame_ex(tx_context_t *ctx,
                        canid_t can_id,
                        uint8_t len,            /* 1..64 (末尾1Bはalive) */
                        int period_ms,
                        const uint8_t *payload, /* payload_len = len-1 */
                        int use_brs);           /* 1:CANFD_BRS */

/* ペイロード更新 */
void tx_update_payload   (tx_context_t *ctx, int index, const uint8_t payload[7]);
void tx_update_payload_ex(tx_context_t *ctx, int index, const uint8_t *payload, uint8_t len);

/* 有効/無効やBRS切替 */
void tx_set_enabled(tx_context_t *ctx, int index, int enabled);
void tx_set_brs    (tx_context_t *ctx, int index, int enable);

#ifdef __cplusplus
}
#endif

#endif /* CAN_MANAGER_H */
