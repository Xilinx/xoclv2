/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for XRT DDR Calibration Leaf Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_CALIB_H_
#define _XRT_CALIB_H_

#include "xleaf.h"
#include <linux/xrt/xclbin.h>

/*
 * Memory calibration driver IOCTL calls.
 */
enum xrt_calib_results {
	XRT_CALIB_UNKNOWN,
	XRT_CALIB_SUCCEEDED,
	XRT_CALIB_FAILED,
};

enum xrt_calib_ioctl_cmd {
	XRT_CALIB_RESULT = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
};

#endif	/* _XRT_CALIB_H_ */
