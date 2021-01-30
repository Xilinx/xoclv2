/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef	_XRT_CLKFREQ_H_
#define	_XRT_CLKFREQ_H_

#include "xleaf.h"

/*
 * CLKFREQ driver IOCTL calls.
 */
enum xrt_clkfreq_ioctl_cmd {
	XRT_CLKFREQ_READ = XRT_XLEAF_CUSTOM_BASE,
};

#endif	/* _XRT_CLKFREQ_H_ */
