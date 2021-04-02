// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver for golden image
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Max Zhen <maxz@xilinx.com>
 */

#include "metadata.h"
#include "xleaf.h"
#include "xleaf/devctl.h"

#define XRT_VSEC_GOLDEN "xrt_vsec_golden"

/*
 * Global static table listing all known devices we need to bring up
 * on all golden images that we need to support.
 */
static struct xrt_golden_endpoint {
	unsigned short vendor;
	unsigned short device;
	struct xrt_md_endpoint ep;
	const char *board_name;
} vsec_golden_eps[] = {
	{
		.vendor = 0x10ee,
		.device = 0xd020,
		.ep = {
			.ep_name = XRT_MD_NODE_FLASH_VSEC,
			.bar_off = 0x1f50000,
			.size = 4096
		},
		.board_name = "u50",
	},
};

/* Version of golden image is read from same location for all Alveo cards. */
static struct xrt_md_endpoint xrt_golden_ver_endpoint = {
	.ep_name = XRT_MD_NODE_GOLDEN_VER,
	.bar_off = 0x131008,
	.size = 4
};

struct xrt_vsec {
	struct xrt_device	*xdev;
	char			*metadata;
	unsigned short		vendor;
	unsigned short		device;
	const char		*bdname;
};

static int xrt_vsec_get_golden_ver(struct xrt_vsec *vsec)
{
	struct xrt_device *devctl_leaf;
	struct xrt_device *xdev = vsec->xdev;
	struct xrt_devctl_rw devctl_arg = { 0 };
	int err, ver;

	devctl_leaf = xleaf_get_leaf_by_epname(xdev, XRT_MD_NODE_GOLDEN_VER);
	if (!devctl_leaf) {
		xrt_err(xdev, "can not get %s", XRT_MD_NODE_GOLDEN_VER);
		return -EINVAL;
	}

	devctl_arg.xdr_id = XRT_DEVCTL_GOLDEN_VER;
	devctl_arg.xdr_buf = &ver;
	devctl_arg.xdr_len = sizeof(ver);
	devctl_arg.xdr_offset = 0;
	err = xleaf_call(devctl_leaf, XRT_DEVCTL_READ, &devctl_arg);
	xleaf_put_leaf(xdev, devctl_leaf);
	if (err) {
		xrt_err(xdev, "can't get golden image version: %d", err);
		return err;
	}

	return ver;
}

static int xrt_vsec_add_node(struct xrt_vsec *vsec, struct xrt_md_endpoint *dev)
{
	int ret;

	xrt_info(vsec->xdev, "add ep %s", dev->ep_name);
	ret = xrt_md_add_endpoint(DEV(vsec->xdev), vsec->metadata, dev);
	if (ret)
		xrt_err(vsec->xdev, "add ep failed, ret %d", ret);
	return ret;
}

static int xrt_vsec_add_all_nodes(struct xrt_vsec *vsec)
{
	int i;
	int rc = -ENOENT;

	for (i = 0; i < ARRAY_SIZE(vsec_golden_eps); i++) {
		struct xrt_golden_endpoint *ep = &vsec_golden_eps[i];

		if (vsec->vendor == ep->vendor && vsec->device == ep->device) {
			rc = xrt_vsec_add_node(vsec, &ep->ep);
			if (rc)
				break;
		}
	}

	if (rc == 0)
		rc = xrt_vsec_add_node(vsec, &xrt_golden_ver_endpoint);

	return rc;
}

static int xrt_vsec_create_metadata(struct xrt_vsec *vsec)
{
	int ret;

	ret = xrt_md_create(&vsec->xdev->dev, &vsec->metadata);
	if (ret) {
		xrt_err(vsec->xdev, "create metadata failed");
		return ret;
	}

	ret = xrt_vsec_add_all_nodes(vsec);
	if (ret) {
		vfree(vsec->metadata);
		vsec->metadata = NULL;
	}
	return ret;
}

static ssize_t VBNV_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xrt_vsec *vsec = xrt_get_drvdata(xdev);

	return sprintf(buf, "xilinx_%s_GOLDEN_%d\n", vsec->bdname,
		xrt_vsec_get_golden_ver(vsec));
}
static DEVICE_ATTR_RO(VBNV);

static struct attribute *vsec_attrs[] = {
	&dev_attr_VBNV.attr,
	NULL,
};

static const struct attribute_group vsec_attrgroup = {
	.attrs = vsec_attrs,
};

static void xrt_vsec_remove(struct xrt_device *xdev)
{
	struct xrt_vsec	*vsec;

	xrt_info(xdev, "leaving...");
	sysfs_remove_group(&DEV(xdev)->kobj, &vsec_attrgroup);
	vsec = xrt_get_drvdata(xdev);
	vfree(vsec->metadata);
}

static int xrt_vsec_probe(struct xrt_device *xdev)
{
	struct xrt_vsec	*vsec;
	int			ret = 0;
	int			i;

	xrt_info(xdev, "probing...");

	vsec = devm_kzalloc(&xdev->dev, sizeof(*vsec), GFP_KERNEL);
	if (!vsec)
		return -ENOMEM;

	vsec->xdev = xdev;
	xleaf_get_root_id(xdev, &vsec->vendor, &vsec->device, NULL, NULL);
	xrt_set_drvdata(xdev, vsec);

	ret = xrt_vsec_create_metadata(vsec);
	if (ret) {
		xrt_err(xdev, "create metadata failed, ret %d", ret);
		goto failed;
	}
	ret = xleaf_create_group(xdev, vsec->metadata);
	if (ret < 0)
		xrt_err(xdev, "create group failed, ret %d", ret);
	else
		ret = 0;

	/* Cache golden board name. */
	for (i = 0; i < ARRAY_SIZE(vsec_golden_eps); i++) {
		struct xrt_golden_endpoint *ep = &vsec_golden_eps[i];

		if (vsec->vendor == ep->vendor && vsec->device == ep->device) {
			vsec->bdname = ep->board_name;
			break;
		}
	}

	if (sysfs_create_group(&DEV(xdev)->kobj, &vsec_attrgroup))
		xrt_err(xdev, "failed to create sysfs group");

failed:
	if (ret)
		xrt_vsec_remove(xdev);

	return ret;
}

static struct xrt_dev_endpoints xrt_vsec_golden_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names []){
			{ .ep_name = XRT_MD_NODE_VSEC_GOLDEN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_driver xrt_vsec_golden_driver = {
	.driver = {
		.name = XRT_VSEC_GOLDEN,
	},
	.subdev_id = XRT_SUBDEV_VSEC_GOLDEN,
	.endpoints = xrt_vsec_golden_endpoints,
	.probe = xrt_vsec_probe,
	.remove = xrt_vsec_remove,
};

XRT_LEAF_INIT_FINI_FUNC(vsec_golden);
