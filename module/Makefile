obj-m += kvmfr.o
USER  := $(shell whoami)

all:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean

test:	all
	grep -q '^uio'   /proc/modules || sudo modprobe uio
	grep -q '^kvmfr' /proc/modules && sudo rmmod kvmfr || true
	sudo insmod ./kvmfr.ko
	sudo chown $(USER) /dev/uio0
	../client/bin/looking-glass-client -f /dev/uio0 -L 128
