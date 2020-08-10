# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m   += xocl-lib.o

fdtdir := ../common/libfdt
xocl-lib-y := 				\
	xocl-main.o			\
	xocl-subdev.o			\
	xocl-cdev.o			\
	../common/xocl-metadata.o	\
	subdevs/xocl-partition.o	\
	subdevs/xocl-test.o		\
	subdevs/xocl-vsec.o		\
	subdevs/xocl-axigate.o		\
	subdevs/xocl-qspi.o		\
	$(fdtdir)/fdt.o			\
	$(fdtdir)/fdt_addresses.o	\
	$(fdtdir)/fdt_empty_tree.o	\
	$(fdtdir)/fdt_overlay.o		\
	$(fdtdir)/fdt_ro.o		\
	$(fdtdir)/fdt_rw.o		\
	$(fdtdir)/fdt_strerror.o	\
	$(fdtdir)/fdt_sw.o		\
	$(fdtdir)/fdt_wip.o

CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))
XILINXINCLUDE := -I$(ROOT)/include -I$(ROOT)/include/uapi -I$(ROOT)/common/libfdt

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
