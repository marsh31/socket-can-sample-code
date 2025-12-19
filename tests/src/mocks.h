// テスト用モック制御インターフェース
#pragma once

#include <sys/timerfd.h>

typedef struct MockConfig {
  // socket 系
  int socket_fail;
  int socket_next_fd; // 返すダミーFD（成功時）

  int setsockopt_fail;

  int fcntl_fail_get;
  int fcntl_fail_set;

  int ioctl_fail;
  int bind_fail;

  int timerfd_create_fail;
  int timerfd_settime_fail;

  // close の呼び出し記録
  int close_count;
  int closed_fds[64];

  // can_tx/rx_* の結果制御＆呼び出し記録
  int can_tx_start_ret; // 0:success, !0:error
  int can_rx_start_ret; // 0:success, !0:error
  int can_tx_stop_calls;
  int can_rx_stop_calls;

  // timerfd_settime の検証用
  int timerfd_settime_calls;
  struct itimerspec last_itimerspec;
  int last_timerfd;
} MockConfig;

extern MockConfig g_mock;

void mock_reset(void);
void mock_record_close(int fd);

