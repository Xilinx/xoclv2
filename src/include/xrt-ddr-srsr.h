/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XOCL_DDR_SRSR_H_
#define _XOCL_DDR_SRSR_H_

#include "xrt-subdev.h"

/*
 * ddr-srsr driver IOCTL calls.
 */
enum xrt_ddr_srsr_ioctl_cmd {
	XOCL_SRSR_FAST_CALIB,
	XOCL_SRSR_CALIB,
	XOCL_SRSR_EP_NAME,
};

struct xrt_srsr_ioctl_calib {
	void	*xsic_buf;
	u32	xsic_size;
	bool	xsic_retention;
};

#endif
