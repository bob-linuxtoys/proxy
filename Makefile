ifneq ($(KERNELRELEASE),)
	obj-m += proxy.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)
 
all:
	make -C $(KERNELDIR) M=$(PWD) modules
 
clean:
	make -C $(KERNELDIR) M=$(PWD) clean
 
install:
	mkdir -p /lib/modules/$(shell uname -r)/kernel/drivers/char
	cp proxy.ko /lib/modules/$(shell uname -r)/kernel/drivers/char
	depmod -a
endif
