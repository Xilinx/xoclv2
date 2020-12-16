# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m   += xrt-lib.o

fdtdir := ../../../../lib
xrt-lib-y := 				\
	xrt-main.o			\
	xrt-subdev.o			\
	xrt-cdev.o			\
	../common/xrt-metadata.o	\
	subdevs/xrt-partition.o	\
	subdevs/xrt-vsec.o		\
	subdevs/xrt-axigate.o		\
	subdevs/xrt-gpio.o		\
	subdevs/xrt-icap.o		\
	subdevs/xrt-clock.o		\
	subdevs/xrt-clkfreq.o		\
	subdevs/xrt-ucs.o		\
	subdevs/xrt-calib.o		\
	$(fdtdir)/fdt.o			\
	$(fdtdir)/fdt_addresses.o	\
	$(fdtdir)/fdt_empty_tree.o	\
	$(fdtdir)/fdt_ro.o		\
	$(fdtdir)/fdt.o			\
	$(fdtdir)/fdt_addresses.o	\
	$(fdtdir)/fdt_empty_tree.o	\
	$(fdtdir)/fdt_ro.o		\
	$(fdtdir)/fdt_rw.o		\
	$(fdtdir)/fdt_strerror.o	\
	$(fdtdir)/fdt_sw.o		\
	$(fdtdir)/fdt_wip.o

CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))/../../../
XILINXINCLUDE := -I$(ROOT)/drivers/fpga/xrt/include -I$(ROOT)/include/uapi -I$(ROOT)/scripts/dtc/libfdt -I$(ROOT)/drivers/fpga/xrt/common

ccflags-y += $(XILINXINCLUDE)
ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG -g -Og
endif

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf *.o *.o.d *~ core .depend .*.cmd *.ko *.ko.unsigned *.mod.c \
	rm -rf subdevs/*.o subdevs/*.o.d subdevs/*~ core subdevs/.depend subdevs/.*.cmd subdevs/*.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk
