# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2019 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m	:= xmgmt.o

xmgmt-y := mgmt-drv.o mgmt-ctx.o

CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))
XILINXINCLUDE := -I$(ROOT)/../include

ccflags-y += $(XILINXINCLUDE) -DPF=USERPF -D__XRT__
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
	.tmp_versions *.symvers modules.order
