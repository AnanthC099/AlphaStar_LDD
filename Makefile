obj-m := pcd.o

KDIR := /lib/modules/$(shell uname -r)/build

all: modules

modules:
	make -C $(KDIR) M=$(shell pwd) modules

clean:
	make -C $(KDIR) M=$(shell pwd) clean
