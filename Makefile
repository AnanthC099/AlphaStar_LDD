obj-m := pcd.o

KDIR := /lib/modules/$(shell uname -r)/build
DTC  := dtc

all: modules dtbo

modules:
	make -C $(KDIR) M=$(shell pwd) modules

dtbo: lcd_jhd162a.dtbo

lcd_jhd162a.dtbo: lcd_jhd162a_overlay.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

clean:
	make -C $(KDIR) M=$(shell pwd) clean
	rm -f lcd_jhd162a.dtbo
