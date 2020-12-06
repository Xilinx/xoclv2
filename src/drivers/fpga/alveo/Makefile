# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

all:
	$(MAKE) -C lib all
	$(MAKE) -C mgmt all
	$(MAKE) lint

tags:
	../scripts/tags.sh

clean:
	$(MAKE) -C lib clean
	$(MAKE) -C mgmt clean

lint:
	../scripts/lint.sh
