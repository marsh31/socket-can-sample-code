#define _GNU_SOURCE
#include "mocks.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// テストで使用するモックのグローバル設定
MockConfig g_mock;

void mock_reset(void)
{
  memset(&g_mock, 0, sizeof(g_mock));
  g_mock.socket_next_fd       = 10; // 任意の正のFDから
  g_mock.can_tx_start_ret     = 0;
  g_mock.can_rx_start_ret     = 0;
  g_mock.timerfd_settime_calls = 0;
  g_mock.last_timerfd         = -1;
}

void mock_record_close(int fd)
{
  if (g_mock.close_count < (int)(sizeof(g_mock.closed_fds) / sizeof(g_mock.closed_fds[0]))) {
    g_mock.closed_fds[g_mock.close_count++] = fd;
  }
}

// ---- ラップ実装群（-Wl,-wrap,...) ----

int __wrap_socket(int domain, int type, int protocol)
{
  (void)domain; (void)type; (void)protocol;
  if (g_mock.socket_fail) {
    errno = EMFILE; // 適当なエラー
    return -1;
  }
  return g_mock.socket_next_fd++;
}

int __wrap_setsockopt(int sockfd, int level, int optname,
                      const void *optval, socklen_t optlen)
{
  (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
  if (g_mock.setsockopt_fail) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int __wrap_fcntl(int fd, int cmd, ...)
{
  (void)fd;
  if (cmd == F_GETFL) {
    if (g_mock.fcntl_fail_get) { errno = EACCES; return -1; }
    return 0; // 既存フラグ無し
  }
  if (cmd == F_SETFL) {
    if (g_mock.fcntl_fail_set) { errno = EACCES; return -1; }
    return 0;
  }
  return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  (void)fd; (void)request;
  if (g_mock.ioctl_fail) { errno = ENODEV; return -1; }
  // ifreq の設定: ifindex を適当に 1 とする
  // 可変引数を介して取り出す
  va_list ap; va_start(ap, request);
  struct ifreq *ifr = va_arg(ap, struct ifreq*);
  va_end(ap);
  if (ifr) ifr->ifr_ifindex = 1;
  return 0;
}

int __wrap_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  (void)sockfd; (void)addr; (void)addrlen;
  if (g_mock.bind_fail) { errno = EADDRNOTAVAIL; return -1; }
  return 0;
}

int __wrap_timerfd_create(int clockid, int flags)
{
  (void)clockid; (void)flags;
  if (g_mock.timerfd_create_fail) { errno = EMFILE; return -1; }
  return g_mock.socket_next_fd++;
}

int __wrap_timerfd_settime(int fd, int flags,
                           const struct itimerspec *new_value,
                           struct itimerspec *old_value)
{
  (void)flags; (void)old_value;
  g_mock.timerfd_settime_calls++;
  g_mock.last_timerfd = fd;
  if (new_value) memcpy(&g_mock.last_itimerspec, new_value, sizeof(*new_value));
  if (g_mock.timerfd_settime_fail) { errno = EINVAL; return -1; }
  return 0;
}

int __wrap_close(int fd)
{
  mock_record_close(fd);
  return 0;
}

// can_tx/can_rx モジュールの開始/停止は start/stop_canfd の単体テストで差し替える
int __wrap_can_tx_start(void *ctx)
{
  (void)ctx;
  return g_mock.can_tx_start_ret;
}

void __wrap_can_tx_stop(void *ctx)
{
  (void)ctx;
  g_mock.can_tx_stop_calls++;
}

int __wrap_can_rx_start(void *ctx)
{
  (void)ctx;
  return g_mock.can_rx_start_ret;
}

void __wrap_can_rx_stop(void *ctx)
{
  (void)ctx;
  g_mock.can_rx_stop_calls++;
}
