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
#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "xocl-gpio.h"

#define XOCL_VSEC_GOLDEN "xocl_vsec_golden"

/*
 * Global static table listing all known devices we need to bring up
 * on all golden images that we need to support.
 */
static struct xocl_golden_endpoint {
	unsigned short vendor;
	unsigned short device;
	struct xocl_md_endpoint ep;
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
static struct xocl_md_endpoint xocl_golden_ver_endpoint = {
	.ep_name = NODE_GOLDEN_VER,
	.bar_off = 0x131008,
	.size = 4
};

struct xocl_vsec {
	struct platform_device	*pdev;
	char			*metadata;
	unsigned short		vendor;
	unsigned short		device;
	const char		*bdname;
};

static int xocl_vsec_get_golden_ver(struct xocl_vsec *vsec)
{
	struct platform_device *gpio_leaf;
	struct platform_device *pdev = vsec->pdev;
	struct xocl_gpio_ioctl_rw gpio_arg = { 0 };
	int err, ver;

	gpio_leaf = xocl_subdev_get_leaf(pdev, xocl_subdev_match_epname,
		NODE_GOLDEN_VER);
	if (!gpio_leaf) {
		xocl_err(pdev, "can not get %s", NODE_GOLDEN_VER);
		return -EINVAL;
	}

	gpio_arg.xgir_id = XOCL_GPIO_GOLDEN_VER;
	gpio_arg.xgir_buf = &ver;
	gpio_arg.xgir_len = sizeof(ver);
	gpio_arg.xgir_offset = 0;
	err = xocl_subdev_ioctl(gpio_leaf, XOCL_GPIO_READ, &gpio_arg);
	(void) xocl_subdev_put_leaf(pdev, gpio_leaf);
	if (err) {
		xocl_err(pdev, "can't get golden image version: %d", err);
		return err;
	}

	return ver;
}

static int xocl_vsec_add_node(struct xocl_vsec *vsec,
	struct xocl_md_endpoint *dev)
{
	int ret;

	xocl_info(vsec->pdev, "add ep %s", dev->ep_name);
	ret = xocl_md_add_endpoint(DEV(vsec->pdev), vsec->metadata, dev);
	if (ret)
		xocl_err(vsec->pdev, "add ep failed, ret %d", ret);
	return ret;
}

static int xocl_vsec_add_all_nodes(struct xocl_vsec *vsec)
{
	int i;
	int rc = -ENOENT;

	for (i = 0; i < ARRAY_SIZE(vsec_golden_eps); i++) {
		struct xocl_golden_endpoint *ep = &vsec_golden_eps[i];

		if (vsec->vendor == ep->vendor && vsec->device == ep->device) {
			rc = xocl_vsec_add_node(vsec, &ep->ep);
			if (rc)
				break;
		}
	}

	if (rc == 0)
		rc = xocl_vsec_add_node(vsec, &xocl_golden_ver_endpoint);

	return rc;
}

static int xocl_vsec_create_metadata(struct xocl_vsec *vsec)
{
	int ret;

	ret = xocl_md_create(&vsec->pdev->dev, &vsec->metadata);
	if (ret) {
		xocl_err(vsec->pdev, "create metadata failed");
		return ret;
	}

	ret = xocl_vsec_add_all_nodes(vsec);
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
	struct xocl_vsec *vsec = platform_get_drvdata(pdev);

	return sprintf(buf, "xilinx_%s_GOLDEN_%d\n", vsec->bdname,
		xocl_vsec_get_golden_ver(vsec));
}
static DEVICE_ATTR_RO(VBNV);

static struct attribute *vsec_attrs[] = {
	&dev_attr_VBNV.attr,
	NULL,
};

static const struct attribute_group vsec_attrgroup = {
	.attrs = vsec_attrs,
};

static int xocl_vsec_remove(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;

	xocl_info(pdev, "leaving...");
	(void) sysfs_remove_group(&DEV(pdev)->kobj, &vsec_attrgroup);
	vsec = platform_get_drvdata(pdev);
	vfree(vsec->metadata);
	return 0;
}

static int xocl_vsec_probe(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;
	int			ret = 0;
	int			i;

	xocl_info(pdev, "probing...");

	vsec = devm_kzalloc(&pdev->dev, sizeof(*vsec), GFP_KERNEL);
	if (!vsec)
		return -ENOMEM;

	vsec->pdev = pdev;
	xocl_subdev_get_parent_id(pdev, &vsec->vendor, &vsec->device,
		NULL, NULL);
	platform_set_drvdata(pdev, vsec);

	ret = xocl_vsec_create_metadata(vsec);
	if (ret) {
		xocl_err(pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}
	ret = xocl_subdev_create_partition(pdev, vsec->metadata);
	if (ret < 0)
		xocl_err(pdev, "create partition failed, ret %d", ret);
	else
		ret = 0;

	/* Cache golden board name. */
	for (i = 0; i < ARRAY_SIZE(vsec_golden_eps); i++) {
		struct xocl_golden_endpoint *ep = &vsec_golden_eps[i];

		if (vsec->vendor == ep->vendor && vsec->device == ep->device) {
			vsec->bdname = ep->board_name;
			break;
		}
	}

	if (sysfs_create_group(&DEV(pdev)->kobj, &vsec_attrgroup))
		xocl_err(pdev, "failed to create sysfs group");

failed:
	if (ret)
		xocl_vsec_remove(pdev);

	return ret;
}

struct xocl_subdev_endpoints xocl_vsec_golden_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names []){
			{ .ep_name = NODE_VSEC_GOLDEN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xocl_subdev_drvdata xocl_vsec_data = {
};

static const struct platform_device_id xocl_vsec_table[] = {
	{ XOCL_VSEC_GOLDEN, (kernel_ulong_t)&xocl_vsec_data },
	{ },
};

struct platform_driver xocl_vsec_golden_driver = {
	.driver = {
		.name = XOCL_VSEC_GOLDEN,
	},
	.probe = xocl_vsec_probe,
	.remove = xocl_vsec_remove,
	.id_table = xocl_vsec_table,
};
