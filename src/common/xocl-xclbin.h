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

/**
 * Bitstream header information.
 */
struct XHwIcap_Bit_Header {
	unsigned int HeaderLength;     /* Length of header in 32 bit words */
	unsigned int BitstreamLength;  /* Length of bitstream to read in bytes*/
	unsigned char *DesignName;     /* Design name get from bitstream */
	unsigned char *PartName;       /* Part name read from bitstream */
	unsigned char *Date;           /* Date read from bitstream header */
	unsigned char *Time;           /* Bitstream creation time*/
	unsigned int MagicLength;      /* Length of the magic numbers*/
};

const char *xrt_xclbin_kind_to_string(enum axlf_section_kind kind);
int xrt_xclbin_get_section(const char *buf,
	enum axlf_section_kind kind, void **data, uint64_t *len);

#endif /* _XOCL_XCLBIN_H */
