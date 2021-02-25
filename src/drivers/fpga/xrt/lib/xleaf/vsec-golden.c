// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver for golden image
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Max Zhen <maxz@xilinx.com>
 */

#include <linux/platform_device.h>
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
	struct platform_device	*pdev;
	char			*metadata;
	unsigned short		vendor;
	unsigned short		device;
	const char		*bdname;
};

static int xrt_vsec_get_golden_ver(struct xrt_vsec *vsec)
{
	struct platform_device *devctl_leaf;
	struct platform_device *pdev = vsec->pdev;
	struct xrt_devctl_ioctl_rw devctl_arg = { 0 };
	int err, ver;

	devctl_leaf = xleaf_get_leaf_by_epname(pdev, XRT_MD_NODE_GOLDEN_VER);
	if (!devctl_leaf) {
		xrt_err(pdev, "can not get %s", XRT_MD_NODE_GOLDEN_VER);
		return -EINVAL;
	}

	devctl_arg.xgir_id = XRT_DEVCTL_GOLDEN_VER;
	devctl_arg.xgir_buf = &ver;
	devctl_arg.xgir_len = sizeof(ver);
	devctl_arg.xgir_offset = 0;
	err = xleaf_ioctl(devctl_leaf, XRT_DEVCTL_READ, &devctl_arg);
	xleaf_put_leaf(pdev, devctl_leaf);
	if (err) {
		xrt_err(pdev, "can't get golden image version: %d", err);
		return err;
	}

	return ver;
}

static int xrt_vsec_add_node(struct xrt_vsec *vsec, struct xrt_md_endpoint *dev)
{
	int ret;

	xrt_info(vsec->pdev, "add ep %s", dev->ep_name);
	ret = xrt_md_add_endpoint(DEV(vsec->pdev), vsec->metadata, dev);
	if (ret)
		xrt_err(vsec->pdev, "add ep failed, ret %d", ret);
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

	ret = xrt_md_create(&vsec->pdev->dev, &vsec->metadata);
	if (ret) {
		xrt_err(vsec->pdev, "create metadata failed");
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
	struct platform_device *pdev = to_platform_device(dev);
	struct xrt_vsec *vsec = platform_get_drvdata(pdev);

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

static int xrt_vsec_remove(struct platform_device *pdev)
{
	struct xrt_vsec	*vsec;

	xrt_info(pdev, "leaving...");
	sysfs_remove_group(&DEV(pdev)->kobj, &vsec_attrgroup);
	vsec = platform_get_drvdata(pdev);
	vfree(vsec->metadata);
	return 0;
}

static int xrt_vsec_probe(struct platform_device *pdev)
{
	struct xrt_vsec	*vsec;
	int			ret = 0;
	int			i;

	xrt_info(pdev, "probing...");

	vsec = devm_kzalloc(&pdev->dev, sizeof(*vsec), GFP_KERNEL);
	if (!vsec)
		return -ENOMEM;

	vsec->pdev = pdev;
	xleaf_get_root_id(pdev, &vsec->vendor, &vsec->device, NULL, NULL);
	platform_set_drvdata(pdev, vsec);

	ret = xrt_vsec_create_metadata(vsec);
	if (ret) {
		xrt_err(pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}
	ret = xleaf_create_group(pdev, vsec->metadata);
	if (ret < 0)
		xrt_err(pdev, "create group failed, ret %d", ret);
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

	if (sysfs_create_group(&DEV(pdev)->kobj, &vsec_attrgroup))
		xrt_err(pdev, "failed to create sysfs group");

failed:
	if (ret)
		xrt_vsec_remove(pdev);

	return ret;
}

static struct xrt_subdev_endpoints xrt_vsec_golden_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = XRT_MD_NODE_VSEC_GOLDEN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_vsec_data = {
};

static const struct platform_device_id xrt_vsec_table[] = {
	{ XRT_VSEC_GOLDEN, (kernel_ulong_t)&xrt_vsec_data },
	{ },
};

static struct platform_driver xrt_vsec_golden_driver = {
	.driver = {
		.name = XRT_VSEC_GOLDEN,
	},
	.probe = xrt_vsec_probe,
	.remove = xrt_vsec_remove,
	.id_table = xrt_vsec_table,
};

void vsec_golden_leaf_init_fini(bool init)
{
	if (init) {
		xleaf_register_driver(XRT_SUBDEV_VSEC_GOLDEN,
				      &xrt_vsec_golden_driver, xrt_vsec_golden_endpoints);
	} else {
		xleaf_unregister_driver(XRT_SUBDEV_VSEC_GOLDEN);
	}
}
