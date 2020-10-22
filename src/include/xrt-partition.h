/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_PARTITION_H_
#define	_XOCL_PARTITION_H_

#include "xrt-subdev.h"

/*
 * Partition driver IOCTL calls.
 */
enum xrt_partition_ioctl_cmd {
	XOCL_PARTITION_GET_LEAF = 0,
	XOCL_PARTITION_PUT_LEAF,
	XOCL_PARTITION_INIT_CHILDREN,
	XOCL_PARTITION_FINI_CHILDREN,
	XOCL_PARTITION_EVENT,
};

struct xrt_partition_ioctl_event {
	enum xrt_events xpie_evt;
	struct xrt_parent_ioctl_evt_cb *xpie_cb;
};

extern int xrt_subdev_parent_ioctl(struct platform_device *pdev,
	u32 cmd, void *arg);

#endif	/* _XOCL_PARTITION_H_ */