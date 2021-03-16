// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA devctl Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/devctl.h"

#define XRT_DEVCTL "xrt_devctl"

struct xrt_name_id {
	char *ep_name;
	int id;
};

static struct xrt_name_id name_id[XRT_DEVCTL_MAX] = {
	{ XRT_MD_NODE_BLP_ROM, XRT_DEVCTL_ROM_UUID },
	{ XRT_MD_NODE_GOLDEN_VER, XRT_DEVCTL_GOLDEN_VER },
};

static const struct regmap_config devctl_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

struct xrt_devctl {
	struct platform_device	*pdev;
	struct regmap		*regmap[XRT_DEVCTL_MAX];
	ulong			sizes[XRT_DEVCTL_MAX];
};

static int xrt_devctl_name2id(struct xrt_devctl *devctl, const char *name)
{
	int i;

	for (i = 0; i < XRT_DEVCTL_MAX && name_id[i].ep_name; i++) {
		if (!strncmp(name_id[i].ep_name, name, strlen(name_id[i].ep_name) + 1))
			return name_id[i].id;
	}

	return -EINVAL;
}

static int
xrt_devctl_leaf_call(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xrt_devctl *devctl;
	int ret = 0;

	devctl = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_DEVCTL_READ: {
		struct xrt_devctl_rw *rw_arg = arg;

		if (rw_arg->xdr_len & 0x3) {
			xrt_err(pdev, "invalid len %d", rw_arg->xdr_len);
			return -EINVAL;
		}

		if (rw_arg->xdr_id >= XRT_DEVCTL_MAX) {
			xrt_err(pdev, "invalid id %d", rw_arg->xdr_id);
			return -EINVAL;
		}

		if (!devctl->regmap[rw_arg->xdr_id]) {
			xrt_err(pdev, "io not found, id %d",
				rw_arg->xdr_id);
			return -EINVAL;
		}

		ret = regmap_bulk_read(devctl->regmap[rw_arg->xdr_id], rw_arg->xdr_offset,
				       rw_arg->xdr_buf,
				       rw_arg->xdr_len / devctl_regmap_config.reg_stride);
		break;
	}
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xrt_devctl_probe(struct platform_device *pdev)
{
	struct xrt_devctl *devctl = NULL;
	void __iomem *base = NULL;
	struct resource *res;
	int i, id, ret = 0;

	devctl = devm_kzalloc(&pdev->dev, sizeof(*devctl), GFP_KERNEL);
	if (!devctl)
		return -ENOMEM;

	devctl->pdev = pdev;
	platform_set_drvdata(pdev, devctl);

	xrt_info(pdev, "probing...");
	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		struct regmap_config config = devctl_regmap_config;

		id = xrt_devctl_name2id(devctl, res->name);
		if (id < 0) {
			xrt_err(pdev, "ep %s not found", res->name);
			continue;
		}
		base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(base)) {
			ret = PTR_ERR(base);
			break;
		}
		config.max_register = res->end - res->start + 1;
		devctl->regmap[id] = devm_regmap_init_mmio(&pdev->dev, base, &config);
		if (IS_ERR(devctl->regmap[id])) {
			xrt_err(pdev, "map base failed %pR", res);
			ret = PTR_ERR(devctl->regmap[id]);
			break;
		}
		devctl->sizes[id] = res->end - res->start + 1;
	}

	return ret;
}

static struct xrt_subdev_endpoints xrt_devctl_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			/* add name if ep is in same partition */
			{ .ep_name = XRT_MD_NODE_BLP_ROM },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_GOLDEN_VER },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	/* adding ep bundle generates devctl device instance */
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_devctl_data = {
	.xsd_dev_ops = {
		.xsd_leaf_call = xrt_devctl_leaf_call,
	},
};

static const struct platform_device_id xrt_devctl_table[] = {
	{ XRT_DEVCTL, (kernel_ulong_t)&xrt_devctl_data },
	{ },
};

static struct platform_driver xrt_devctl_driver = {
	.driver = {
		.name = XRT_DEVCTL,
	},
	.probe = xrt_devctl_probe,
	.id_table = xrt_devctl_table,
};

void devctl_leaf_init_fini(bool init)
{
	if (init)
		xleaf_register_driver(XRT_SUBDEV_DEVCTL, &xrt_devctl_driver, xrt_devctl_endpoints);
	else
		xleaf_unregister_driver(XRT_SUBDEV_DEVCTL);
}
