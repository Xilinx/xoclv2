/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_PARTITION_H_
#define	_XRT_PARTITION_H_

/*
 * Partition driver IOCTL calls.
 */
enum xrt_partition_ioctl_cmd {
	XRT_PARTITION_GET_LEAF = XRT_XLEAF_CUSTOM_BASE,
	XRT_PARTITION_PUT_LEAF,
	XRT_PARTITION_INIT_CHILDREN,
	XRT_PARTITION_FINI_CHILDREN,
	XRT_PARTITION_TRIGGER_EVENT,
};

#endif	/* _XRT_PARTITION_H_ */
