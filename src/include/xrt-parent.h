/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_PARENT_H_
#define	_XRT_PARENT_H_

#include "xrt-subdev.h"
#include "xrt-partition.h"

/*
 * Parent IOCTL calls.
 */
enum xrt_parent_ioctl_cmd {
	/* Leaf actions. */
	XRT_PARENT_GET_LEAF = 0,
	XRT_PARENT_PUT_LEAF,
	XRT_PARENT_GET_LEAF_HOLDERS,

	/* Partition actions. */
	XRT_PARENT_CREATE_PARTITION,
	XRT_PARENT_REMOVE_PARTITION,
	XRT_PARENT_LOOKUP_PARTITION,
	XRT_PARENT_WAIT_PARTITION_BRINGUP,

	/* Event actions. */
	XRT_PARENT_ADD_EVENT_CB,
	XRT_PARENT_REMOVE_EVENT_CB,
	XRT_PARENT_ASYNC_BOARDCAST_EVENT,

	/* Device info. */
	XRT_PARENT_GET_RESOURCE,
	XRT_PARENT_GET_ID,

	/* Misc. */
	XRT_PARENT_HOT_RESET,
	XRT_PARENT_HWMON,
};

struct xrt_parent_ioctl_get_leaf {
	struct platform_device *xpigl_pdev; /* caller's pdev */
	xrt_subdev_match_t xpigl_match_cb;
	void *xpigl_match_arg;
	struct platform_device *xpigl_leaf; /* target leaf pdev */
};

struct xrt_parent_ioctl_put_leaf {
	struct platform_device *xpipl_pdev; /* caller's pdev */
	struct platform_device *xpipl_leaf; /* target's pdev */
};

struct xrt_parent_ioctl_lookup_partition {
	struct platform_device *xpilp_pdev; /* caller's pdev */
	xrt_subdev_match_t xpilp_match_cb;
	void *xpilp_match_arg;
	int xpilp_part_inst;
};

struct xrt_parent_ioctl_evt_cb {
	struct platform_device *xevt_pdev; /* caller's pdev */
	xrt_subdev_match_t xevt_match_cb;
	void *xevt_match_arg;
	xrt_event_cb_t xevt_cb;
	void *xevt_hdl;
};

struct xrt_parent_ioctl_async_broadcast_evt {
	struct platform_device *xaevt_pdev; /* caller's pdev */
	enum xrt_events xaevt_event;
	xrt_async_broadcast_event_cb_t xaevt_cb;
	void *xaevt_arg;
};

struct xrt_parent_ioctl_get_holders {
	struct platform_device *xpigh_pdev; /* caller's pdev */
	char *xpigh_holder_buf;
	size_t xpigh_holder_buf_len;
};

struct xrt_parent_ioctl_get_res {
	struct resource *xpigr_res;
};

struct xrt_parent_ioctl_get_id {
	unsigned short  xpigi_vendor_id;
	unsigned short  xpigi_device_id;
	unsigned short  xpigi_sub_vendor_id;
	unsigned short  xpigi_sub_device_id;
};

struct xrt_parent_ioctl_hwmon {
	bool xpih_register;
	const char *xpih_name;
	void *xpih_drvdata;
	const struct attribute_group **xpih_groups;
	struct device *xpih_hwmon_dev;
};

#endif	/* _XRT_PARENT_H_ */
