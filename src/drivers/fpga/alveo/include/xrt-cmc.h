/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_CMC_H_
#define	_XRT_CMC_H_

#include "xrt-subdev.h"
#include <linux/xrt/xclbin.h>

/*
 * CMC driver IOCTL calls.
 */
enum xrt_cmc_ioctl_cmd {
	XRT_CMC_READ_BOARD_INFO = 0,
	XRT_CMC_READ_SENSORS,
};

#endif	/* _XRT_CMC_H_ */
