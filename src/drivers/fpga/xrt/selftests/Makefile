# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2021 Xilinx, Inc. All rights reserved.
#
# Authors:
#     Sonal.Santan@xilinx.com
#

obj-m	+= xrt-stest1.o

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

xrt-stest1-y := selftest1.o		\
	   selftest1-main.o		\
	   xleaf/test.o			\
	   $(fdtobj)


CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))/../../../
XILINXINCLUDE := -I$(ROOT)/drivers/fpga/xrt/include -I$(ROOT)/include/uapi -I$(ROOT)/scripts/dtc/libfdt

ccflags-y += $(XILINXINCLUDE) -Wall -Werror -Wmissing-prototypes -Wunused-but-set-variable -Wold-style-declaration

ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG -Og -g
endif

all:
	$(MAKE) -C $(KERNELDIR) W=1 M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(PWD)/../lib/Module.symvers modules

clean:
	rm -rf *.o *.o.d *~ core .depend .*.cmd *.ko *.ko.unsigned *.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk \
	xleaf/*.o xleaf/*.o.d xleaf/*~ xleaf/.*.cmd \
	$(fdtdir)/*.o
