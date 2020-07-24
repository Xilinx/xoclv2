# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

obj-m   += xocl-lib.o
xocl-lib-y := 				\
	xocl-main.o			\
	xocl-subdev.o			\
	xocl-cdev.o			\
	subdevs/xocl-partition.o	\
	subdevs/xocl-test.o

CONFIG_MODULE_SIG=n
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD	:= $(shell pwd)
ROOT	:= $(dir $(M))
XILINXINCLUDE := -I$(ROOT)/include -I$(ROOT)/include/uapi

ccflags-y += $(XILINXINCLUDE)
ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG -g -Og
endif

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	rm -rf *.o *.o.d *~ core .depend .*.cmd *.ko *.ko.unsigned *.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk
