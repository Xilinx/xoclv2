/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XOCL_CLOCK_H_
#define	_XOCL_CLOCK_H_

#include "xocl-subdev.h"
#include "xclbin.h"

/*
 * CLOCK driver IOCTL calls.
 */
enum xocl_clock_ioctl_cmd {
	XOCL_CLOCK_SET = 0,
	XOCL_CLOCK_VERIFY,
};

#endif	/* _XOCL_CLOCK_H_ */
