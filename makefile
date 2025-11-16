

all: can_demo_send can_demo_recv can_demo_bcm_send

can_demo_recv: can_demo_recv.c
	gcc -Wall -O2 -o can_demo_recv can_demo_recv.c

can_demo_send: can_demo_send.c
	gcc -Wall -O2 -o can_demo_send can_demo_send.c

can_demo_bcm_send: can_demo_bcm_send.c
	gcc -Wall -O2 -o can_demo_bcm_send can_demo_bcm_send.c

run:
	./can_demo

clean:
	rm can_demo_send can_demo_recv can_demo_bcm_send
