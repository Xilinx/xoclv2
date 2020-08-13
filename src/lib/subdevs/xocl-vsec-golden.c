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

#define XOCL_VSEC_GOLDEN "xocl_vsec_golden"

static struct xocl_golden_offsets {
	const char *ep_name;
	unsigned short vendor;
	unsigned short device;
	u64 offset;
} vsec_golden_offsets[] = {
	{ NODE_DRV_FLASH, 0x10ee, 0xd020, 0x1f50000 },
};

static struct xocl_md_endpoint vsec_devs[] = {
	{
		.ep_name = NODE_DRV_FLASH,
		.bar = 0,
		.bar_off = 0,
		.size = 4096,
	},
};

struct xocl_vsec {
	struct platform_device	*pdev;
	char			*metadata;
	unsigned short		vendor;
	unsigned short		device;
};

static int xocl_vsec_fill_dev_node(struct xocl_vsec *vsec,
	struct xocl_md_endpoint *dev)
{
	int i;

	dev->bar_off = -1;
	for (i = 0; i < ARRAY_SIZE(vsec_golden_offsets); i++) {
		struct xocl_golden_offsets *offs = &vsec_golden_offsets[i];

		if (strcmp(dev->ep_name, offs->ep_name) == 0 &&
			vsec->vendor == offs->vendor &&
			vsec->device == offs->device) {
			dev->bar_off = offs->offset;
			break;
		}
	}

	return dev->bar_off == -1 ? -EINVAL : 0;
}

static int xocl_vsec_add_node(struct xocl_vsec *vsec,
	struct xocl_md_endpoint *dev)
{
	int ret;

	xocl_info(vsec->pdev, "add ep %s", dev->ep_name);
	ret = xocl_md_add_endpoint(DEV(vsec->pdev), &vsec->metadata, dev);
	if (ret)
		xocl_err(vsec->pdev, "add ep failed, ret %d", ret);
	return ret;
}

static int xocl_vsec_create_metadata(struct xocl_vsec *vsec)
{
	int i;
	int ret;

	ret = xocl_md_create(&vsec->pdev->dev, &vsec->metadata);
	if (ret) {
		xocl_err(vsec->pdev, "create metadata failed");
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(vsec_devs); i++) {
		struct xocl_md_endpoint *vd = &vsec_devs[i];

		ret = xocl_vsec_fill_dev_node(vsec, vd);
		if (ret)
			break;
		ret = xocl_vsec_add_node(vsec, vd);
		if (ret)
			break;
	}
	if (ret) {
		vfree(vsec->metadata);
		vsec->metadata = NULL;
	}
	return ret;
}

static int xocl_vsec_remove(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;

	xocl_info(pdev, "leaving...");
	vsec = platform_get_drvdata(pdev);
	vfree(vsec->metadata);
	return 0;
}

static int xocl_vsec_probe(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;
	int			ret = 0;

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
