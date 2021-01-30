/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Xilinx Kernel Driver XCLBIN parser
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *    David Zhang <davidzha@xilinx.com>
 *    Sonal Santan <sonal.santan@xilinx.com>
 */

#ifndef _XRT_XCLBIN_H
#define _XRT_XCLBIN_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/xrt/xclbin.h>

#define	ICAP_XCLBIN_V2	"xclbin2"
#define DMA_HWICAP_BITFILE_BUFFER_SIZE 1024
#define MAX_XCLBIN_SIZE (1024 * 1024 * 1024) /* Assuming xclbin <= 1G, always */

enum axlf_section_kind;
struct axlf;

/**
 * Bitstream header information as defined by Xilinx tools.
 * Please note that this struct definition is not owned by the driver and
 * hence it does not use Linux coding style.
 */
struct hw_icap_bit_header {
	unsigned int header_length;     /* Length of header in 32 bit words */
	unsigned int bitstream_length;  /* Length of bitstream to read in bytes*/
	unsigned char *design_name;     /* Design name get from bitstream */
	unsigned char *part_name;       /* Part name read from bitstream */
	unsigned char *date;           /* Date read from bitstream header */
	unsigned char *time;           /* Bitstream creation time*/
	unsigned int magic_length;      /* Length of the magic numbers*/
};

const char *xrt_xclbin_kind_to_string(enum axlf_section_kind kind);
int xrt_xclbin_get_section(const struct axlf *xclbin,
			   enum axlf_section_kind kind, void **data,
			   uint64_t *len);
int xrt_xclbin_get_metadata(struct device *dev, const struct axlf *xclbin, char **dtb);
int xrt_xclbin_parse_bitstream_header(const unsigned char *data,
				      unsigned int size,
				      struct hw_icap_bit_header *header);
void xrt_xclbin_free_header(struct hw_icap_bit_header *header);
const char *xrt_clock_type2epname(enum CLOCK_TYPE type);

#endif /* _XRT_XCLBIN_H */
