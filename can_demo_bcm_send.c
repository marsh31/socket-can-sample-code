#include <errno.h>
#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/raw.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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

  struct {
    struct bcm_msg_head msg_head;
    struct canfd_frame frame[2];
  } msg;
  memset(&msg, 0, sizeof(msg));

  msg.msg_head.opcode  = TX_SETUP;
  msg.msg_head.flags   = SETTIMER | STARTTIMER | CAN_FD_FRAME;
  msg.msg_head.can_id  = 0x123;
  msg.msg_head.nframes = 1;
  msg.msg_head.count   = 0;  // 0 = 無限送信

  msg.msg_head.ival1.tv_sec  = 1;  // 最初の送信までの遅延時間
  msg.msg_head.ival1.tv_usec = 0;  // 最初の送信までの遅延時間

  msg.msg_head.ival2.tv_sec  = 1;  // 送信周期
  msg.msg_head.ival2.tv_usec = 0;  // 送信周期

  msg.frame[0].can_id = 0x0123;
  msg.frame[0].len    = 8;
  msg.frame[0].flags  = CANFD_BRS;
  for (int i = 0; i < msg.frame[0].len; i++) {
    msg.frame[0].data[i] = (unsigned char)i;
  }

  msg.frame[1].can_id = 0x0124;
  msg.frame[1].len    = 4;
  msg.frame[1].flags  = CANFD_BRS;
  for (int i = 0; i < msg.frame[1].len; i++) {
    msg.frame[1].data[i] = (unsigned char)i;
  }

  ssize_t nbytes = send(s, &msg, sizeof(msg), 0);
  if (nbytes < 0) {
    perror("Write TX_SETUP");
    close(s);
    return 1;
  }

  printf("TX_SETUP sent.\n");
  printf("vcan0 上に ID 0x123 を 1000ms 周期で送信中です。\n");
  printf("candump vcan0 などで確認できます。\n");
  printf("終了するには Enter キーを押してください。\n");

  // ユーザ入力待ち（その間もカーネルが送信を続ける）
  getchar();

  struct bcm_msg_head del;
  memset(&del, 0, sizeof(del));
  del.opcode = TX_DELETE;
  del.can_id = 0x123;

  nbytes = write(s, &del, sizeof(del));
  if (nbytes < 0) {
    perror("write TX_DELETE");
    // 終了はする
  } else {
    printf("TX_DELETE sent. Periodic transmission stopped.\n");
  }

  close(s);
  return 0;
}
