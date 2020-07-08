# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m   += xocl-lib.o
#obj-m   += ptest.o

xocl-lib-y := xocl-from.o xocl-xmc.o xocl-icap.o xocl-icap-ioctl.o xocl-region.o xocl-core.o
#ptest-y := ptest.o

CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))
XILINXINCLUDE := -I$(ROOT)/include -I$(ROOT)/include/uapi

ccflags-y += $(XILINXINCLUDE) -DPF=MGMTPF -D__XRT__
ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG
endif

ifeq ($(SYMBOL),1)
ccflags-y += -g
endif

all:
	echo $(PWD)
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf *.o *.o.d *~ core .depend .*.cmd *.ko *.ko.unsigned *.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk
