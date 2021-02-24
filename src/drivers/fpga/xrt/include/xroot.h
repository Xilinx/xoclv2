/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header file for Xilinx Runtime (XRT) driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef _XRT_ROOT_H_
#define _XRT_ROOT_H_

#include <linux/pci.h>
#include "subdev_id.h"
#include "events.h"

typedef bool (*xrt_subdev_match_t)(enum xrt_subdev_id,
	struct platform_device *, void *);
#define XRT_SUBDEV_MATCH_PREV	((xrt_subdev_match_t)-1)
#define XRT_SUBDEV_MATCH_NEXT	((xrt_subdev_match_t)-2)

/*
 * Root IOCTL calls.
 */
enum xrt_root_ioctl_cmd {
	/* Leaf actions. */
	XRT_ROOT_GET_LEAF = 0,
	XRT_ROOT_PUT_LEAF,
	XRT_ROOT_GET_LEAF_HOLDERS,

	/* Group actions. */
	XRT_ROOT_CREATE_GROUP,
	XRT_ROOT_REMOVE_GROUP,
	XRT_ROOT_LOOKUP_GROUP,
	XRT_ROOT_WAIT_GROUP_BRINGUP,

	/* Event actions. */
	XRT_ROOT_EVENT,
	XRT_ROOT_EVENT_ASYNC,

	/* Device info. */
	XRT_ROOT_GET_RESOURCE,
	XRT_ROOT_GET_ID,

	/* Misc. */
	XRT_ROOT_HOT_RESET,
	XRT_ROOT_HWMON,
};

struct xrt_root_ioctl_get_leaf {
	struct platform_device *xpigl_pdev; /* caller's pdev */
	xrt_subdev_match_t xpigl_match_cb;
	void *xpigl_match_arg;
	struct platform_device *xpigl_leaf; /* target leaf pdev */
};

struct xrt_root_ioctl_put_leaf {
	struct platform_device *xpipl_pdev; /* caller's pdev */
	struct platform_device *xpipl_leaf; /* target's pdev */
};

struct xrt_root_ioctl_lookup_group {
	struct platform_device *xpilp_pdev; /* caller's pdev */
	xrt_subdev_match_t xpilp_match_cb;
	void *xpilp_match_arg;
	int xpilp_grp_inst;
};

struct xrt_root_ioctl_get_holders {
	struct platform_device *xpigh_pdev; /* caller's pdev */
	char *xpigh_holder_buf;
	size_t xpigh_holder_buf_len;
};

struct xrt_root_ioctl_get_res {
	struct resource *xpigr_res;
};

struct xrt_root_ioctl_get_id {
	unsigned short  xpigi_vendor_id;
	unsigned short  xpigi_device_id;
	unsigned short  xpigi_sub_vendor_id;
	unsigned short  xpigi_sub_device_id;
};

struct xrt_root_ioctl_hwmon {
	bool xpih_register;
	const char *xpih_name;
	void *xpih_drvdata;
	const struct attribute_group **xpih_groups;
	struct device *xpih_hwmon_dev;
};

typedef int (*xrt_subdev_root_cb_t)(struct device *, void *, u32, void *);
int xrt_subdev_root_request(struct platform_device *self, u32 cmd, void *arg);

/*
 * Defines physical function (MPF / UPF) specific operations
 * needed in common root driver.
 */
struct xroot_pf_cb {
	void (*xpc_hot_reset)(struct pci_dev *pdev);
};

int xroot_probe(struct pci_dev *pdev, struct xroot_pf_cb *cb, void **root);
void xroot_remove(void *root);
bool xroot_wait_for_bringup(void *root);
int xroot_add_vsec_node(void *root, char *dtb);
int xroot_create_group(void *xr, char *dtb);
int xroot_add_simple_node(void *root, char *dtb, const char *endpoint);
void xroot_broadcast(void *root, enum xrt_events evt);

#endif	/* _XRT_ROOT_H_ */
