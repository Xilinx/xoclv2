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
	lib-drv.o		\
	xroot.o			\
	xclbin.o		\
	subdev.o		\
	cdev.o			\
	group.o			\
	xleaf/vsec.o		\
	xleaf/vsec-golden.o	\
	xleaf/axigate.o		\
	xleaf/qspi.o		\
	xleaf/devctl.o		\
	xleaf/mailbox.o		\
	xleaf/icap.o		\
	xleaf/cmc.o		\
	xleaf/cmc-ctrl.o	\
	xleaf/cmc-sensors.o	\
	xleaf/cmc-mailbox.o	\
	xleaf/cmc-bdinfo.o	\
	xleaf/cmc-sc.o		\
	xleaf/srsr.o		\
	xleaf/clock.o		\
	xleaf/clkfreq.o		\
	xleaf/ucs.o		\
	xleaf/ddr_calibration.o	\
	xleaf/pcie-firewall.o	\
	$(fdtobj)

ifndef CONFIG_FPGA_XRT_METADATA
xrt-lib-y += ../metadata/metadata.o
endif

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
	rm -rf xleaf/*.o xleaf/*.o.d xleaf/*~ core xleaf/.depend xleaf/.*.cmd xleaf/*.mod.c \
	.tmp_versions *.symvers modules.order *.mod .cache.mk ../metadata/metadata.o \
	../metadata/.metadata.o.cmd ../metadata/.metadata.o.d
