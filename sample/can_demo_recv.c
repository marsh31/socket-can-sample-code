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

  printf("waiting for can fd frames on vcan...\n");

  while (1) {
    ssize_t nbytes = read(s, &frame, sizeof(struct canfd_frame));
    if (nbytes < 0) {
      perror("read error");
      break;
    }

    if (nbytes < (ssize_t)sizeof(struct canfd_frame)) {
      if (nbytes < (ssize_t)sizeof(struct can_frame)) {
        fprintf(
            stderr, "incomplete CAN frame / CAN FD frame %zd bytes\n", nbytes);
        continue;
      }

      printf("Received CAN frame: can id=0x%X, len=%d, data=",
             frame.can_id,
             frame.len);

      for (int i = 0; i < frame.len; i++) {
        printf("%02X ", frame.data[i]);
      }
      printf("\n");
      continue;
    }

    printf("Received CAN FD frame: can id=0x%X, len=%d, data=",
           frame.can_id,
           frame.len);

    for (int i = 0; i < frame.len; i++) {
      printf("%02X ", frame.data[i]);
    }
    printf("\n");
  }

  close(s);
  return 0;
}
