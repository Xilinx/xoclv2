/* SPDX-License-Identifier: GPL-2.0 */
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
	/* Always the 1st. */
	XOCL_PART_BEGIN = 0,

	XOCL_PART_TEST = XOCL_PART_BEGIN,
	XOCL_PART_TEST_1,

	/* Always in the end. */
	XOCL_PART_END
};

/*
 * Partition driver IOCTL calls.
 */
enum xocl_partition_ioctl_cmd {
	XOCL_PARTITION_GET_LEAF = 0,
	XOCL_PARTITION_PUT_LEAF,
};

#endif	/* _XOCL_PARTITION_H_ */
