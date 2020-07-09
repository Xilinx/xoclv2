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

echo "Generating Emacs TAGS file $out from $ROOTSRC ..."
ALL_FILES=$(git ls-files --exclude-standard --full-name $ROOTSRC)
for item in $ALL_FILES;
do
    FILES=("${FILES[@]}$ROOT/$item\n")
done
echo -e ${FILES[@]} | ctags --totals -e -L - -f $out
