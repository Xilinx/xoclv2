# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com
#

all:    fast
	$(MAKE) lint

fast:
	$(MAKE) -C lib all
	$(MAKE) -C mgnt all
	$(MAKE) -C selftests all

tags:
	../../../../scripts/tags.sh

clean:
	$(MAKE) -C lib clean
	$(MAKE) -C mgnt clean
	$(MAKE) -C selftests clean
	rm -f ../../../lib/.fdt*.o.cmd

lint:
	../../../../scripts/lint.sh

test:	all
	../../../../scripts/load.sh
