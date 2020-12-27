# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m   += xrt-lib.o

fdtdir := ../../../../lib

ifndef CONFIG_LIBFDT
fdtobj := 				\
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
endif

xrt-lib-y := 			\
	main.o			\
	root.o			\
	xclbin.o		\
	metadata.o		\
	subdev.o		\
	cdev.o			\
	partition.o		\
	subdevs/test.o		\
	subdevs/vsec.o		\
	subdevs/vsec-golden.o	\
	subdevs/axigate.o	\
	subdevs/qspi.o		\
	subdevs/gpio.o		\
	subdevs/mailbox.o	\
	subdevs/icap.o		\
	subdevs/cmc.o		\
	subdevs/cmc-ctrl.o	\
	subdevs/cmc-sensors.o	\
	subdevs/cmc-mailbox.o	\
	subdevs/cmc-bdinfo.o	\
	subdevs/cmc-sc.o	\
	subdevs/srsr.o		\
	subdevs/clock.o		\
	subdevs/clkfreq.o	\
	subdevs/ucs.o		\
	subdevs/calib.o		\
	$(fdtobj)


CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))/../../../
XILINXINCLUDE := -I$(ROOT)/drivers/fpga/xrt/include -I$(ROOT)/include/uapi -I$(ROOT)/scripts/dtc/libfdt

ccflags-y += $(XILINXINCLUDE) -Wall -Werror -Wmissing-prototypes -Wunused-but-set-variable -Wold-style-declaration

ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG -g -Og
endif

all:
	$(MAKE) -C $(KERNELDIR) W=1 M=$(PWD) modules

clean:
	rm -rf *.o *.o.d *~ core .depend .*.cmd *.ko *.ko.unsigned *.mod.c \
	rm -rf subdevs/*.o subdevs/*.o.d subdevs/*~ core subdevs/.depend subdevs/.*.cmd subdevs/*.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk
