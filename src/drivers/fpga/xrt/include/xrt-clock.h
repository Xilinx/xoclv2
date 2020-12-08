/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XRT_CLOCK_H_
#define	_XRT_CLOCK_H_

#include "xrt-subdev.h"
#include <linux/xrt/xclbin.h>

/*
 * CLOCK driver IOCTL calls.
 */
enum xrt_clock_ioctl_cmd {
	XRT_CLOCK_SET = 0,
	XRT_CLOCK_GET,
	XRT_CLOCK_VERIFY,
};

struct xrt_clock_ioctl_get {
	u16 freq;
	u32 freq_cnter;
};

#endif	/* _XRT_CLOCK_H_ */
