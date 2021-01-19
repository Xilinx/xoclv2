# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2020 Xilinx, Inc. All rights reserved.
#
# Authors: Cheng Zhen <maxz@xilinx.com>
#
IDIR=../../src/include/uapi
CC=gcc
CFLAGS=-I$(IDIR)

_DEPS = xrt-test.h
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ = main.o
OBJ = $(patsubst %,%,$(_OBJ))

LIBS=

OUTPUT=test.exe

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: $(OBJ)
	$(CC) -o $(OUTPUT) $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f *.o $(OUTPUT)
