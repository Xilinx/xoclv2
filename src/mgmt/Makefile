# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2019-2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m	+= xmgmt.o

commondir := ../common
fdtdir := $(commondir)/libfdt
xmgmt-y := xmgmt-root.o				\
	   xmgmt-main.o				\
	   $(commondir)/xocl-root.o		\
	   $(commondir)/xocl-metadata.o		\
	   $(commondir)/xocl-xclbin.o		\
	   $(fdtdir)/fdt.o			\
	   $(fdtdir)/fdt_addresses.o		\
	   $(fdtdir)/fdt_empty_tree.o		\
	   $(fdtdir)/fdt_overlay.o		\
	   $(fdtdir)/fdt_ro.o			\
	   $(fdtdir)/fdt_rw.o			\
	   $(fdtdir)/fdt_strerror.o		\
	   $(fdtdir)/fdt_sw.o			\
	   $(fdtdir)/fdt_wip.o


CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))
XILINXINCLUDE := -I$(ROOT)/include -I$(ROOT)/include/uapi -I$(ROOT)/common/libfdt -I$(ROOT)/common

ccflags-y += $(XILINXINCLUDE)
ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG -Og -g
endif

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(PWD)/../lib/Module.symvers modules

clean:
	rm -rf *.o *.o.d *~ core .depend .*.cmd *.ko *.ko.unsigned *.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk \
	$(fdtdir)/*.o $(commondir)/*.o
