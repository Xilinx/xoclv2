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
#include "xrt-metadata.h"
#include "xrt-subdev.h"
#include "xrt-gpio.h"

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
			.ep_name = NODE_FLASH_VSEC,
			.bar_off = 0x1f50000,
			.size = 4096
		},
		.board_name = "u50",
	},
};

/* Version of golden image is read from same location for all Alveo cards. */
static struct xrt_md_endpoint xrt_golden_ver_endpoint = {
	.ep_name = NODE_GOLDEN_VER,
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
	struct platform_device *gpio_leaf;
	struct platform_device *pdev = vsec->pdev;
	struct xrt_gpio_ioctl_rw gpio_arg = { 0 };
	int err, ver;

	gpio_leaf = xrt_subdev_get_leaf_by_epname(pdev, NODE_GOLDEN_VER);
	if (!gpio_leaf) {
		xrt_err(pdev, "can not get %s", NODE_GOLDEN_VER);
		return -EINVAL;
	}

	gpio_arg.xgir_id = XRT_GPIO_GOLDEN_VER;
	gpio_arg.xgir_buf = &ver;
	gpio_arg.xgir_len = sizeof(ver);
	gpio_arg.xgir_offset = 0;
	err = xrt_subdev_ioctl(gpio_leaf, XRT_GPIO_READ, &gpio_arg);
	(void) xrt_subdev_put_leaf(pdev, gpio_leaf);
	if (err) {
		xrt_err(pdev, "can't get golden image version: %d", err);
		return err;
	}

	return ver;
}

static int xrt_vsec_add_node(struct xrt_vsec *vsec,
	struct xrt_md_endpoint *dev)
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

static ssize_t VBNV_show(struct device *dev,
	struct device_attribute *da, char *buf)
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
	(void) sysfs_remove_group(&DEV(pdev)->kobj, &vsec_attrgroup);
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
	xrt_subdev_get_parent_id(pdev, &vsec->vendor, &vsec->device,
		NULL, NULL);
	platform_set_drvdata(pdev, vsec);

	ret = xrt_vsec_create_metadata(vsec);
	if (ret) {
		xrt_err(pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}
	ret = xrt_subdev_create_partition(pdev, vsec->metadata);
	if (ret < 0)
		xrt_err(pdev, "create partition failed, ret %d", ret);
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

struct xrt_subdev_endpoints xrt_vsec_golden_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = NODE_VSEC_GOLDEN },
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

struct platform_driver xrt_vsec_golden_driver = {
	.driver = {
		.name = XRT_VSEC_GOLDEN,
	},
	.probe = xrt_vsec_probe,
	.remove = xrt_vsec_remove,
	.id_table = xrt_vsec_table,
};
