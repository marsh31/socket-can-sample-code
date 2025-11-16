#include <errno.h>
#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/raw.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int setup_tx(int s,
             int ifindex,
             canid_t can_id,
             unsigned int period_ms,
             unsigned int len,
             uint8_t base_val)
{
  struct {
    struct bcm_msg_head msg_head;
    struct canfd_frame frame[1];
  } msg;
  memset(&msg, 0, sizeof(msg));

  msg.msg_head.opcode  = TX_SETUP;
  msg.msg_head.flags   = SETTIMER | STARTTIMER | CAN_FD_FRAME;
  msg.msg_head.can_id  = can_id;
  msg.msg_head.nframes = 1;
  msg.msg_head.count   = 0;  // 0 = 無限送信

  msg.msg_head.ival1.tv_sec  = 0;  // 最初の送信までの遅延時間
  msg.msg_head.ival1.tv_usec = 0;  // 最初の送信までの遅延時間

  msg.msg_head.ival2.tv_sec  = period_ms / 1000;           // 送信周期
  msg.msg_head.ival2.tv_usec = (period_ms % 1000) * 1000;  // 送信周期

  msg.frame[0].can_id = can_id;
  msg.frame[0].len    = len;
  msg.frame[0].flags  = CANFD_BRS;
  for (int i = 0; i < msg.frame[0].len; i++) {
    msg.frame[0].data[i] = (unsigned char)i;
  }

  ssize_t nbytes = send(s, &msg, sizeof(msg), 0);
  if (nbytes < 0) {
    perror("Write TX_SETUP");
    close(s);
    return -1;
  }

  return 0;
}


int update_fd_data(int s, canid_t can_id, __u8 *new_data, __u8 len)
{
  struct {
    struct bcm_msg_head msg_head;
    struct canfd_frame frame[1];
  } upd;
  memset(&upd, 0, sizeof(upd));

  upd.msg_head.opcode  = TX_SETUP;
  upd.msg_head.flags   = CAN_FD_FRAME;
  upd.msg_head.can_id  = can_id;
  upd.msg_head.nframes = 1;

  upd.frame[0].can_id = can_id;
  upd.frame[0].len    = len;
  upd.frame[0].flags  = CANFD_BRS;

  memcpy(upd.frame[0].data, new_data, len);

  ssize_t nbytes = send(s, &upd, sizeof(upd), 0);
  if (nbytes < 0) {
    perror("TX_SETUP(update)");
    return -1;
  }

  return 0;
}

int main()
{
  int s;
  struct sockaddr_can addr;
  struct ifreq ifr;

  s = socket(PF_CAN, SOCK_DGRAM, CAN_BCM);
  if (s < 0) {
    perror("socket open is failed");
    return 1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, "vcan0");
  if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
    perror("ioctl SIOCGIFINDEX");
    close(s);
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.can_family  = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(s);
    return 1;
  }

  if (setup_tx(s, ifr.ifr_ifindex, 0x123, 1000, 8, 0x00) < 0) {
    close(s);
    return 1;
  }

  if (setup_tx(s, ifr.ifr_ifindex, 0x456, 500, 16, 0x00) < 0) {
    close(s);
    return 1;
  }

  printf("TX_SETUP sent.\n");
  printf("vcan0 上に ID 0x123 を 1000ms 周期で送信中です。\n");
  printf("candump vcan0 などで確認できます。\n");
  printf("終了するには Enter キーを押してください。\n");


  int cnt = 0;
  while (1) {
    usleep(100000);

    cnt++;
    __u8 new_payload[8] = {cnt, cnt, 0, 0, cnt, cnt, 0, 0};
    update_fd_data(s, 0x123, new_payload, 8);

    if (cnt > 1000000) {
      cnt = 0;
    }
  }

  // ユーザ入力待ち（その間もカーネルが送信を続ける）
  getchar();

  struct bcm_msg_head del;
  memset(&del, 0, sizeof(del));

  del.opcode = TX_DELETE;
  del.can_id = 0x123;
  if (send(s, &del, sizeof(del), 0) < 0) {
    perror("tx delete 0x123");
  }

  memset(&del, 0, sizeof(del));

  del.opcode = TX_DELETE;
  del.can_id = 0x456;
  if (send(s, &del, sizeof(del), 0) < 0) {
    perror("tx delete 0x456");
  }

  close(s);
  return 0;
}
