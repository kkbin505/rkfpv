# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build of the IMX462 driver and the Radxa Zero 3 overlay.
#   on the board:  sudo ./install.sh   (or: make && sudo make install)
#   cross:         make KDIR=/path/to/rockchip/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

obj-m := imx462.o

KDIR ?= /lib/modules/$(shell uname -r)/build
DTC ?= dtc
OVERLAY := radxa-zero3-imx462

all: module dtbo

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

dtbo: $(OVERLAY).dtbo

$(OVERLAY).dtbo: $(OVERLAY).dts
	$(DTC) -@ -q -I dts -O dtb -o $@ $<

install:
	./install.sh

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f $(OVERLAY).dtbo

.PHONY: all module dtbo install clean
