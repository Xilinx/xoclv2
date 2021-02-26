/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_DDR_SRSR_H_
#define _XRT_DDR_SRSR_H_

#include "xleaf.h"

/*
 * ddr-srsr driver leaf calls.
 */
enum xrt_ddr_srsr_leaf_cmd {
	XRT_SRSR_FAST_CALIB = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	XRT_SRSR_CALIB,
	XRT_SRSR_EP_NAME,
};

struct xrt_srsr_calib {
	void	*xsic_buf;
	u32	xsic_size;
	bool	xsic_retention;
};

#endif
