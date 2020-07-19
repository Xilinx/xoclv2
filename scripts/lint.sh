#!/bin/bash

# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2019 Xilinx, Inc. All rights reserved.
#
# Authors: Sonal.Santan@xilinx.com

set -e

ROOT=`git rev-parse --show-toplevel`
ROOTSRC="$ROOT/src"
out=TAGS

echo "Running checkpatch on source files in $ROOTSRC ..."
ALL_FILES=$(git ls-files --exclude-standard --full-name $ROOTSRC)
for item in $ALL_FILES;
do
    # Skip all the libfdt files which are part of Linux kernel
    regex="^.+libfdt.*$"
    if [[ $item =~ $regex ]]; then
	continue
    fi
    # Skip all the "ref" files which are for reference only
    regex="^src\/ref\/.*$"
    if [[ $item =~ $regex ]]; then
	continue
    fi
    regex="^.+[c|h]$"
    if [[ $item =~ $regex ]]; then
	$ROOT/scripts/checkpatch.pl --no-tree --emacs --color=never -f $ROOT/$item || true
    fi
done
