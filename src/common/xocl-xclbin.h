// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Kernel Driver XCLBIN parser
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors: David Zhang <davidzha@xilinx.com>
 */

#ifndef _XOCL_XCLBIN_H
#define _XOCL_XCLBIN_H

#include <linux/types.h>
#include "xclbin.h"

enum axlf_section_kind;
struct axlf;

const char *xrt_xclbin_kind_to_string(enum axlf_section_kind kind);
int xrt_xclbin_get_section(const char *buf,
	enum axlf_section_kind kind, void **data, uint64_t *len);

#endif /* _XOCL_XCLBIN_H */
