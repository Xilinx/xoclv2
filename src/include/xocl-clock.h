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
};

struct xocl_clock_desc {
	char	*clock_ep_name;
	u32	clock_xclbin_type;
	char	*clkfreq_ep_name;
};

extern struct xocl_clock_desc clock_desc[];

#endif	/* _XOCL_CLOCK_H_ */
