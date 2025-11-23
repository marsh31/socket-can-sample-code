/*
 *
 *
 *
 */

#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H


/*
 *
 */
typedef struct tx_context_t tx_context_t;


/*
 *
 */
typedef struct tx_object_t tx_object_t;


/*
 *
 */
extern tx_context_t *open_canfd(const char *ifname);


/*
 *
 */
extern void close_canfd(tx_context_t *ctx);


/*
 *
 */
extern int start_canfd(tx_context_t *ctx);


/*
 *
 */
extern void stop_canfd(tx_context_t *ctx);


/*
 *
 */
extern int add_canfd_frame(tx_context_t *ctx,
                           canid_t can_id,
                           uint8_t dlc,
                           int period_ms,
                           const uint8_t init_payload);


#endif /* CAN_MANAGER_H */
