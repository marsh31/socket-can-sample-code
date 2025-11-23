

all: can_demo_send can_demo_recv can_demo_bcm_send can_manager_single_id can_manager_multi_id

can_demo_recv: can_demo_recv.c
	gcc -Wall -O2 -o can_demo_recv can_demo_recv.c

can_demo_send: can_demo_send.c
	gcc -Wall -O2 -o can_demo_send can_demo_send.c

can_demo_bcm_send: can_demo_bcm_send.c
	gcc -Wall -O2 -o can_demo_bcm_send can_demo_bcm_send.c

can_manager_single_id: ./can_manager_single_id.c
	gcc -Wall -O2 -o can_manager can_manager_single_id.c

can_manager_multi_id: ./can_manager_multi_id.c
	gcc -Wall -O2 -o can_manager_multi_id can_manager_multi_id.c

run:
	./can_demo

clean:
	rm can_demo_send can_demo_recv can_demo_bcm_send can_manager_single_id can_manager_multi_id
