# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

all:
	$(MAKE) -C lib all
	$(MAKE) -C mgmt all
	$(MAKE) -C selftests all
	$(MAKE) lint

tags:
	../../../../scripts/tags.sh

clean:
	$(MAKE) -C lib clean
	$(MAKE) -C mgmt clean
	$(MAKE) -C selftests clean
	rm -f ../../../lib/.fdt*.o.cmd

lint:
	../../../../scripts/lint.sh

test:	all
	../../../../scripts/load.sh
