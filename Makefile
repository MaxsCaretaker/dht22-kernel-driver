# Makefile – dht22_driver kernel module
#
# Native compile on your Pi 4:
#   make
#   make load     ← insmod + quick status check
#
# Cross-compile from an x86 host (optional):
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/rpi-kernel

obj-m := dht22_driver.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
ARCH          ?=
CROSS_COMPILE ?=

# -DDEBUG turns on all pr_debug() calls in the driver.
# Remove it once you're happy with the behaviour.
ccflags-y := -DDEBUG

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod dht22_driver.ko
	@echo ""
	@echo "── dmesg tail ──────────────────────────────"
	@dmesg | tail -8
	@echo ""
	@echo "── device node ─────────────────────────────"
	@ls -l /dev/dht22 2>/dev/null || echo "  /dev/dht22 not created – check dmesg"

unload:
	sudo rmmod dht22_driver

reload: unload load

status:
	@lsmod | grep dht22 || echo "Module not loaded"
	@ls -l /dev/dht22 2>/dev/null || echo "/dev/dht22 missing"
	@dmesg | grep dht22 | tail -10

# Quick one-shot read test without Python
test:
	sudo cat /dev/dht22

.PHONY: all clean load unload reload status test
