#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
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
  struct canfd_frame frame;
  int enable_canfd = 1;

  s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (s < 0) {
    perror("socket open is failed");
    return 1;
  }

  if (setsockopt(s,
                 SOL_CAN_RAW,
                 CAN_RAW_FD_FRAMES,
                 &enable_canfd,
                 sizeof(enable_canfd))
      < 0) {
    perror("setsocketopt CAN_RAW_FD_FRAMES is failed");
    close(s);
    return 1;
  }

  strcpy(ifr.ifr_name, "vcan0");
  if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
    perror("ioctl SIOCGIFINDEX");
    close(s);
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.can_family  = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(s);
    return 1;
  }

  memset(&frame, 0, sizeof(frame));
  frame.can_id = 0x123;
  frame.len    = 64;
  for (int i = 0; i < frame.len; i++) {
    frame.data[i] = (unsigned char)i;
  }

  ssize_t nbytes = write(s, &frame, sizeof(struct canfd_frame));
  if (nbytes < 0) {
    perror("write");
    close(s);
    return 1;
  } else if (nbytes < (ssize_t)sizeof(struct canfd_frame)) {
    fprintf(stderr, "partial CAN FD frame written: %zd bytes\n", nbytes);
    close(s);
    return 1;
  }

  printf("Sent CAN FD frame: can id=0x%X, len=%d\n", frame.can_id, frame.len);

  close(s);
  return 0;
}
