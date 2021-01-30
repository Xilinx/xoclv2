// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA GPIO Driver
 *
 * Copyright (C) 2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/gpio.h"

#define XRT_GPIO "xrt_gpio"

struct xrt_name_id {
	char *ep_name;
	int id;
};

static struct xrt_name_id name_id[XRT_GPIO_MAX] = {
	{ NODE_BLP_ROM, XRT_GPIO_ROM_UUID },
	{ NODE_GOLDEN_VER, XRT_GPIO_GOLDEN_VER },
};

struct xrt_gpio {
	struct platform_device	*pdev;
	void		__iomem *base_addrs[XRT_GPIO_MAX];
	ulong			sizes[XRT_GPIO_MAX];
};

static int xrt_gpio_name2id(struct xrt_gpio *gpio, const char *name)
{
	int	i;

	for (i = 0; i < XRT_GPIO_MAX && name_id[i].ep_name; i++) {
		if (!strncmp(name_id[i].ep_name, name, strlen(name_id[i].ep_name) + 1))
			return name_id[i].id;
	}

	return -EINVAL;
}

static int
xrt_gpio_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xrt_gpio	*gpio;
	int			ret = 0;

	gpio = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_GPIO_READ: {
		struct xrt_gpio_ioctl_rw	*rw_arg = arg;
		u32				*p_src, *p_dst, i;

		if (rw_arg->xgir_len & 0x3) {
			xrt_err(pdev, "invalid len %d", rw_arg->xgir_len);
			return -EINVAL;
		}

		if (rw_arg->xgir_id >= XRT_GPIO_MAX) {
			xrt_err(pdev, "invalid id %d", rw_arg->xgir_id);
			return -EINVAL;
		}

		p_src = gpio->base_addrs[rw_arg->xgir_id];
		if (!p_src) {
			xrt_err(pdev, "io not found, id %d",
				rw_arg->xgir_id);
			return -EINVAL;
		}
		if (rw_arg->xgir_offset + rw_arg->xgir_len >
		    gpio->sizes[rw_arg->xgir_id]) {
			xrt_err(pdev, "invalid argument, off %d, len %d",
				rw_arg->xgir_offset, rw_arg->xgir_len);
			return -EINVAL;
		}
		p_dst = rw_arg->xgir_buf;
		for (i = 0; i < rw_arg->xgir_len / sizeof(u32); i++) {
			u32 val = ioread32(p_src + rw_arg->xgir_offset + i);

			memcpy(p_dst + i, &val, sizeof(u32));
		}
		break;
	}
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xrt_gpio_remove(struct platform_device *pdev)
{
	struct xrt_gpio	*gpio;
	int			i;

	gpio = platform_get_drvdata(pdev);

	for (i = 0; i < XRT_GPIO_MAX; i++) {
		if (gpio->base_addrs[i])
			iounmap(gpio->base_addrs[i]);
	}

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, gpio);

	return 0;
}

static int xrt_gpio_probe(struct platform_device *pdev)
{
	struct xrt_gpio	*gpio;
	int			i, id, ret = 0;
	struct resource		*res;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->pdev = pdev;
	platform_set_drvdata(pdev, gpio);

	xrt_info(pdev, "probing...");
	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		id = xrt_gpio_name2id(gpio, res->name);
		if (id < 0) {
			xrt_err(pdev, "ep %s not found", res->name);
			continue;
		}
		gpio->base_addrs[id] = ioremap(res->start, res->end - res->start + 1);
		if (!gpio->base_addrs[id]) {
			xrt_err(pdev, "map base failed %pR", res);
			ret = -EIO;
			goto failed;
		}
		gpio->sizes[id] = res->end - res->start + 1;
	}

failed:
	if (ret)
		xrt_gpio_remove(pdev);

	return ret;
}

struct xrt_subdev_endpoints xrt_gpio_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			/* add name if ep is in same partition */
			{ .ep_name = NODE_BLP_ROM },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = NODE_GOLDEN_VER },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	/* adding ep bundle generates gpio device instance */
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_gpio_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_gpio_leaf_ioctl,
	},
};

static const struct platform_device_id xrt_gpio_table[] = {
	{ XRT_GPIO, (kernel_ulong_t)&xrt_gpio_data },
	{ },
};

struct platform_driver xrt_gpio_driver = {
	.driver = {
		.name = XRT_GPIO,
	},
	.probe = xrt_gpio_probe,
	.remove = xrt_gpio_remove,
	.id_table = xrt_gpio_table,
};
