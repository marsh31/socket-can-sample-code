#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mocks.h"
#include "can_internal.h"   // テスト用に内部構造へアクセス
#include "can_manager.h"

// 簡易テストユーティリティ
static int g_total = 0;
static int g_failed = 0;

#define TEST(name) static void name(void)

#define CHECK(cond) do { \
  ++g_total; \
  if (!(cond)) { \
    ++g_failed; \
    fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
  } \
} while(0)

#define CHECK_EQ_INT(a,b) CHECK((int)(a) == (int)(b))
#define CHECK_EQ_UINT(a,b) CHECK((unsigned)(a) == (unsigned)(b))

// -------------------- テスト本体 --------------------

// 正常系: 拡張IDフラグの付与
// 何をテスト: can_extended_format が CAN_EFF_FLAG を確実に付与する
TEST(test_can_extended_format_sets_flag)
{
  canid_t id = 0x123u;
  canid_t ext = can_extended_format(id);
  CHECK((ext & CAN_EFF_FLAG) != 0);
  CHECK_EQ_UINT(ext & 0x1FFFFFFF, id & 0x1FFFFFFF);
}

// 正常系: add_canfd_frame (従来CAN, 8byte, 周期=ベースの倍数)
// 何をテスト: 正常登録・tick_div/フラグ/ペイロードの初期化
TEST(test_add_canfd_frame_standard_ok)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t payload[8] = {1,2,3,4,5,6,7,8};
  int idx = add_canfd_frame(&ctx, 0x100, 8, 20, payload, 0, 0, 0, NULL, NULL);
  CHECK_EQ_INT(idx, 0);
  CHECK_EQ_INT(ctx.num_objs, 1);
  CHECK_EQ_UINT(ctx.objs[0].can_id, 0x100);
  CHECK_EQ_INT(ctx.objs[0].dlc, 8);
  CHECK_EQ_INT(ctx.objs[0].tick_div, 2);
  CHECK(ctx.objs[0].fd_flags == 0);
  CHECK(memcmp(ctx.objs[0].payload, payload, 8) == 0);
}

// 境界値: CAN FD で len>64 を 64 に丸める
// 何をテスト: len=70 指定時に dlc=64 へ丸められること
TEST(test_add_canfd_frame_fd_len_trim_to_64)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t payload[70]; memset(payload, 0xAA, sizeof(payload));
  int idx = add_canfd_frame(&ctx, can_extended_format(0x12345), 70, 20,
                            payload, 1, 0, 1, NULL, NULL);
  CHECK_EQ_INT(idx, 0);
  CHECK_EQ_INT(ctx.objs[0].dlc, 64);
}

// 異常系: 非FDで len>8 はエラー
// 何をテスト: use_fdf=0 かつ len=12 で -1 を返し、num_objs が増えない
TEST(test_add_canfd_frame_nonfd_len_gt8_ng)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t payload[12] = {0};
  int idx = add_canfd_frame(&ctx, 0x200, 12, 20, payload, 0, 0, 0, NULL, NULL);
  CHECK_EQ_INT(idx, -1);
  CHECK_EQ_INT(ctx.num_objs, 0);
}

// 異常系: 周期が0/倍数でない
// 何をテスト: period_ms<=0 および baseの非倍数で -1 を返す
TEST(test_add_canfd_frame_invalid_period)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t payload[8] = {0};
  CHECK_EQ_INT(add_canfd_frame(&ctx, 0x100, 8, 0, payload, 0, 0, 0, NULL, NULL), -1);
  CHECK_EQ_INT(add_canfd_frame(&ctx, 0x100, 8, 15, payload, 0, 0, 0, NULL, NULL), -1);
}

// 境界値: MAX_TX_OBJECTS を超える登録は失敗
// 何をテスト: MAX_TX_OBJECTS 個は成功し、次が -1
TEST(test_add_canfd_frame_max_objects_limit)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t payload[8] = {0};
  int ok = 1;
  for (int i = 0; i < MAX_TX_OBJECTS; ++i) {
    int idx = add_canfd_frame(&ctx, 0x300 + i, 8, 10, payload, 0, 0, 0, NULL, NULL);
    if (idx != i) ok = 0;
  }
  CHECK(ok);
  int ng = add_canfd_frame(&ctx, 0x399, 8, 10, payload, 0, 0, 0, NULL, NULL);
  CHECK_EQ_INT(ng, -1);
}

// 正常系: tx_update_payload_by_index（コールバック無し → 安全コピー）
// 何をテスト: len < dlc で指定分のみ更新、それ以外は不変
TEST(test_tx_update_payload_by_index_copy)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t init[8] = {0};
  (void)add_canfd_frame(&ctx, 0x400, 8, 10, init, 0, 0, 0, NULL, NULL);

  uint8_t upd[5] = {9,8,7,6,5};
  tx_update_payload_by_index(&ctx, 0, upd, 5);

  CHECK(memcmp(ctx.objs[0].payload, upd, 5) == 0);
  for (int i = 5; i < 8; ++i) CHECK_EQ_INT(ctx.objs[0].payload[i], 0);
}

// 正常系: tx_update_payload_by_index（update_cb 経由で更新）
// 何をテスト: update_cb が呼ばれ、payload がコールバックの仕様通りに変更
static void update_cb_copy_plus1(canid_t id, uint8_t dlc, uint8_t *dst, const uint8_t *src)
{
  (void)id; for (uint8_t i = 0; i < dlc; ++i) dst[i] = (uint8_t)(src[i] + 1);
}

TEST(test_tx_update_payload_by_index_with_cb)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t init[8] = {0};
  (void)add_canfd_frame(&ctx, 0x401, 8, 10, init, 0, 0, 0, update_cb_copy_plus1, NULL);
  uint8_t upd[8] = {1,2,3,4,5,6,7,8};
  tx_update_payload_by_index(&ctx, 0, upd, 8);
  for (int i = 0; i < 8; ++i) CHECK_EQ_INT(ctx.objs[0].payload[i], upd[i] + 1);
}

// 正常系: tx_update_payload_by_id（対象IDのみ更新）
// 何をテスト: by_id が一致するオブジェクトにのみ反映
TEST(test_tx_update_payload_by_id_targets_only)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t z8[8] = {0};
  (void)add_canfd_frame(&ctx, 0x500, 8, 10, z8, 0, 0, 0, NULL, NULL);
  (void)add_canfd_frame(&ctx, 0x501, 8, 10, z8, 0, 0, 0, NULL, NULL);
  uint8_t upd[3] = {0xAA,0xBB,0xCC};
  tx_update_payload_by_id(&ctx, 0x501, upd, 3);
  CHECK(memcmp(ctx.objs[1].payload, upd, 3) == 0);
  CHECK(ctx.objs[0].payload[0] == 0);
}

// 異常系: tx_update_payload_* の引数が不正でも落ちない
// 何をテスト: NULL ctx / NULL payload / 不正index で安全に無視
TEST(test_tx_update_payload_invalid_args_no_crash)
{
  tx_update_payload_by_index(NULL, 0, (const uint8_t*)"x", 1);
  can_context_t ctx = {0};
  uint8_t v = 1;
  tx_update_payload_by_index(&ctx, -1, &v, 1);
  tx_update_payload_by_id(&ctx, 0x1, NULL, 1);
  // ここまで例外なく来ればOK
  CHECK(1);
}

// 正常系: tx_set_enabled / tx_set_brs
// 何をテスト: 有効/無効の切替、BRSフラグの付与/除去、範囲外は無視
TEST(test_tx_set_enabled_and_brs)
{
  can_context_t ctx = {0};
  ctx.base_period_ms = 10;
  uint8_t z8[8] = {0};
  (void)add_canfd_frame(&ctx, 0x600, 8, 10, z8, 0, 0, 1, NULL, NULL);
  tx_set_enabled(&ctx, 0, 0);
  CHECK_EQ_INT(ctx.objs[0].enabled, 0);
  tx_set_enabled(&ctx, 0, 1);
  CHECK_EQ_INT(ctx.objs[0].enabled, 1);
  tx_set_brs(&ctx, 0, 1);
  CHECK((ctx.objs[0].fd_flags & CANFD_BRS) == CANFD_BRS);
  tx_set_brs(&ctx, 0, 0);
  CHECK((ctx.objs[0].fd_flags & CANFD_BRS) == 0);
  // 範囲外
  tx_set_enabled(&ctx, 9, 0);
  tx_set_brs(&ctx, -1, 1);
  CHECK(1);
}

// 正常系: set_canfd_rx_callback
// 何をテスト: コールバック/ユーザデータの格納
static void dummy_rx(const struct canfd_frame *f, void *user)
{ (void)f; (void)user; }

TEST(test_set_canfd_rx_callback_assigns)
{
  can_context_t ctx = {0};
  set_canfd_rx_callback(&ctx, dummy_rx, (void*)0x1234);
  CHECK(ctx.rx_cb == dummy_rx);
  CHECK(ctx.rx_user == (void*)0x1234);
  set_canfd_rx_callback(&ctx, NULL, NULL);
  CHECK(ctx.rx_cb == NULL);
}

// 正常系: open_canfd 成功 → close_canfd で close が呼ばれる
// 何をテスト: ソケット/タイマFD の作成・設定成功時にコンテキストが生成され、
//             close_canfd で両FDが close されること
TEST(test_open_close_canfd_success)
{
  mock_reset();
  can_context_t *ctx = open_canfd("vcan0", 10);
  CHECK(ctx != NULL);
  int sock = ctx->sock; int tfd = ctx->tfd;
  close_canfd(ctx);
  int seen_sock = 0, seen_tfd = 0;
  for (int i = 0; i < g_mock.close_count; ++i) {
    if (g_mock.closed_fds[i] == sock) seen_sock = 1;
    if (g_mock.closed_fds[i] == tfd)  seen_tfd  = 1;
  }
  CHECK(seen_sock && seen_tfd);
}

// 異常系: open_canfd 途中失敗（各ステージ）
// 何をテスト: socket/setsockopt/ioctl/bind/timerfd系の失敗で NULL を返し、
//             必要に応じて close が呼ばれる
TEST(test_open_canfd_fail_paths)
{
  mock_reset();
  g_mock.socket_fail = 1; CHECK(open_canfd("vcan0", 10) == NULL);

  mock_reset();
  g_mock.setsockopt_fail = 1;
  can_context_t *ctx = open_canfd("vcan0", 10);
  CHECK(ctx == NULL);
  // setsockopt 失敗時は socket を close しているはず
  CHECK(g_mock.close_count >= 1);

  mock_reset(); g_mock.ioctl_fail = 1; CHECK(open_canfd("vcan0", 10) == NULL);
  mock_reset(); g_mock.bind_fail = 1;  CHECK(open_canfd("vcan0", 10) == NULL);
  mock_reset(); g_mock.timerfd_create_fail = 1; CHECK(open_canfd("vcan0", 10) == NULL);
  mock_reset(); g_mock.timerfd_settime_fail = 1; CHECK(open_canfd("vcan0", 10) == NULL);
}

// 正常系/異常系: start_canfd（tx成功, rx失敗は許容）/tx失敗
// 何をテスト: tx成功で0を返し running=1 のまま、tx失敗で-1かつ running=0
TEST(test_start_canfd_paths)
{
  mock_reset();
  can_context_t ctx = {0};
  // tx:成功, rx:失敗(許容)
  g_mock.can_tx_start_ret = 0;
  g_mock.can_rx_start_ret = -1;
  CHECK_EQ_INT(start_canfd(&ctx), 0);
  CHECK_EQ_INT(ctx.running, 1);

  // tx:失敗
  mock_reset();
  can_context_t ctx2 = {0};
  g_mock.can_tx_start_ret = -1;
  CHECK_EQ_INT(start_canfd(&ctx2), -1);
  CHECK_EQ_INT(ctx2.running, 0);
}

// 正常系: stop_canfd
// 何をテスト: running=1 の状態でタイマを最短化し、tx/rx stop が呼ばれる
TEST(test_stop_canfd_calls_modules_and_timer)
{
  mock_reset();
  can_context_t ctx = {0};
  ctx.running = 1;
  ctx.tfd = 123;
  stop_canfd(&ctx);
  CHECK_EQ_INT(g_mock.timerfd_settime_calls, 1);
  CHECK_EQ_INT(g_mock.last_timerfd, 123);
  CHECK_EQ_INT(g_mock.can_tx_stop_calls, 1);
  CHECK_EQ_INT(g_mock.can_rx_stop_calls, 1);
  CHECK_EQ_INT(ctx.running, 0);
  // itimerspec 検証: ほぼ即時 (nsec=1)
  CHECK(g_mock.last_itimerspec.it_value.tv_nsec == 1);
  CHECK(g_mock.last_itimerspec.it_interval.tv_nsec == 1);
}

int main(void)
{
  printf("[RUN] can_manager unit tests\n");
  // 実行
  test_can_extended_format_sets_flag();
  test_add_canfd_frame_standard_ok();
  test_add_canfd_frame_fd_len_trim_to_64();
  test_add_canfd_frame_nonfd_len_gt8_ng();
  test_add_canfd_frame_invalid_period();
  test_add_canfd_frame_max_objects_limit();
  test_tx_update_payload_by_index_copy();
  test_tx_update_payload_by_index_with_cb();
  test_tx_update_payload_by_id_targets_only();
  test_tx_update_payload_invalid_args_no_crash();
  test_tx_set_enabled_and_brs();
  test_set_canfd_rx_callback_assigns();
  test_open_close_canfd_success();
  test_open_canfd_fail_paths();
  test_start_canfd_paths();
  test_stop_canfd_calls_modules_and_timer();

  printf("[DONE] total=%d failed=%d\n", g_total, g_failed);
  return g_failed ? 1 : 0;
}
