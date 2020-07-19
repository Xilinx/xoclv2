// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_PARTITION_H_
#define	_XOCL_PARTITION_H_

#include "xocl-subdev.h"

/*
 * Defines all flavors of partitions. This also serves as instance ID for
 * partition subdev. An instance of partition subdev can be identified by
 * <XOCL_SUBDEV_PART, xocl_partition_id>.
 */
enum xocl_partition_id {
	XOCL_PART_TEST = 0,
	XOCL_PART_TEST_1,
};

/*
 * Partition driver IOCTL calls.
 */
enum xocl_partition_ioctl_cmd {
	XOCL_PARTITION_GET_LEAF = 0,
	XOCL_PARTITION_PUT_LEAF,
};

struct xocl_partition_ioctl_get_leaf {
	struct platform_device *xpart_pdev; /* caller's pdev */
	enum xocl_subdev_id xpart_id;
	xocl_leaf_match_t xpart_match_cb;
	u64 xpart_match_arg;
	struct platform_device *xpart_leaf; /* target leaf pdev */
};

#endif	/* _XOCL_PARTITION_H_ */
