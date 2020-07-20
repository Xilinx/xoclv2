// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 * 	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_PARENT_H_
#define	_XOCL_PARENT_H_

#include "xocl-subdev.h"
#include "xocl-partition.h"

/*
 * Parent IOCTL calls.
 */
enum xocl_parent_ioctl_cmd {
	XOCL_PARENT_GET_LEAF = 0,
	XOCL_PARENT_GET_LEAF_BY_ID,
	XOCL_PARENT_PUT_LEAF,
	XOCL_PARENT_CREATE_PARTITION,
	XOCL_PARENT_REMOVE_PARTITION,
};

struct xocl_parent_ioctl_get_leaf {
	struct platform_device *xpigl_pdev; /* caller's pdev */
	xocl_subdev_match_t xpigl_match_cb;
	u64 xpigl_match_arg;
	struct platform_device *xpigl_leaf; /* target leaf pdev */
};

struct xocl_parent_ioctl_get_leaf_by_id {
	struct platform_device *xpiglbi_pdev; /* caller's pdev */
	enum xocl_subdev_id xpiglbi_id;
	int xpiglbi_instance;
	struct platform_device *xpiglbi_leaf; /* target leaf pdev */
};

struct xocl_parent_ioctl_create_partition {
	enum xocl_partition_id xpicp_id;
	void *xpicp_dtb;
};

struct xocl_parent_ioctl_put_leaf {
	struct platform_device *xpipl_pdev; /* caller's pdev */
	struct platform_device *xpipl_leaf; /* target's pdev */
};

#endif	/* _XOCL_PARENT_H_ */
