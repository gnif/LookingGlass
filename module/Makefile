obj-m += kvmfr.o
USER  := $(shell whoami)
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

test:
	gcc test.c -Wall -Werror -g -Og -o test	

load: all
	grep -q '^uio'   /proc/modules || sudo modprobe uio
	grep -q '^kvmfr' /proc/modules && sudo rmmod kvmfr || true
	sudo insmod ./kvmfr.ko
	sudo chown $(USER) /dev/uio0
	sudo chown $(USER) /dev/kvmfr0

.PHONY: test
