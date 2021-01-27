/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_GROUP_H_
#define	_XRT_GROUP_H_

#include "xleaf.h"

/*
 * Group driver IOCTL calls.
 */
enum xrt_group_ioctl_cmd {
	XRT_GROUP_GET_LEAF = XRT_XLEAF_CUSTOM_BASE,
	XRT_GROUP_PUT_LEAF,
	XRT_GROUP_INIT_CHILDREN,
	XRT_GROUP_FINI_CHILDREN,
	XRT_GROUP_TRIGGER_EVENT,
};

#endif	/* _XRT_GROUP_H_ */
