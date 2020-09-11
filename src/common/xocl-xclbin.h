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
#include <linux/device.h>
#include "xclbin.h"

#define	ICAP_XCLBIN_V2	"xclbin2"
#define DMA_HWICAP_BITFILE_BUFFER_SIZE 1024

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
int xrt_xclbin_get_section(const char *xclbin,
	enum axlf_section_kind kind, void **data, uint64_t *len);
int xrt_xclbin_get_metadata(struct device *dev, const char *xclbin, char **dtb);
int xrt_xclbin_parse_header(const unsigned char *data,
	unsigned int size, struct XHwIcap_Bit_Header *header);
void xrt_xclbin_free_header(struct XHwIcap_Bit_Header *header);

#endif /* _XOCL_XCLBIN_H */
