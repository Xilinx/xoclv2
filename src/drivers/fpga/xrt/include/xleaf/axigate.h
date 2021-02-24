/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for XRT Axigate Leaf Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_AXIGATE_H_
#define _XRT_AXIGATE_H_

#include "xleaf.h"
#include "metadata.h"

/*
 * AXIGATE driver IOCTL calls.
 */
enum xrt_axigate_ioctl_cmd {
	XRT_AXIGATE_FREEZE = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
	XRT_AXIGATE_FREE,
};

#endif	/* _XRT_AXIGATE_H_ */
