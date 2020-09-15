// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA GPIO Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "xocl-parent.h"
#include "xocl-gpio.h"

#define XOCL_GPIO "xocl_gpio"

struct xocl_name_id {
	char *ep_name;
	int id;
};

static struct xocl_name_id name_id[XOCL_GPIO_MAX] = {
	{ NODE_BLP_ROM, XOCL_GPIO_ROM_UUID },
	{ NODE_GOLDEN_VER, XOCL_GPIO_GOLDEN_VER },
};

struct xocl_gpio {
	struct platform_device	*pdev;
	void		__iomem *base_addrs[XOCL_GPIO_MAX];
	ulong			sizes[XOCL_GPIO_MAX];
};

static int xocl_gpio_name2id(struct xocl_gpio *gpio, const char *name)
{
	int	i;

	for (i = 0; i < XOCL_GPIO_MAX && name_id[i].ep_name; i++) {
		if (!strncmp(name_id[i].ep_name, name,
		    strlen(name_id[i].ep_name) + 1))
			return name_id[i].id;
	}

	return -EINVAL;
}

static int
xocl_gpio_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xocl_gpio	*gpio;
	int			ret = 0;

	gpio = platform_get_drvdata(pdev);

	switch (cmd) {
	case XOCL_GPIO_READ: {
		struct xocl_gpio_ioctl_rw	*rw_arg = arg;
		u32				*p_src, *p_dst, i;

		if (rw_arg->xgir_len & 0x3) {
			xocl_err(pdev, "invalid len %d", rw_arg->xgir_len);
			return -EINVAL;
		}

		if (rw_arg->xgir_id >= XOCL_GPIO_MAX) {
			xocl_err(pdev, "invalid id %d", rw_arg->xgir_id);
			return -EINVAL;
		}

		p_src = gpio->base_addrs[rw_arg->xgir_id];
		if (!p_src) {
			xocl_err(pdev, "io not found, id %d",
				rw_arg->xgir_id);
			return -EINVAL;
		}
		if (rw_arg->xgir_offset + rw_arg->xgir_len >
		    gpio->sizes[rw_arg->xgir_id]) {
			xocl_err(pdev, "invalid argument, off %d, len %d",
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
		xocl_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xocl_gpio_remove(struct platform_device *pdev)
{
	struct xocl_gpio	*gpio;
	int			i;

	gpio = platform_get_drvdata(pdev);

	for (i = 0; i < XOCL_GPIO_MAX; i++) {
		if (gpio->base_addrs[i])
			iounmap(gpio->base_addrs[i]);
	}

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, gpio);

	return 0;
}

static int xocl_gpio_probe(struct platform_device *pdev)
{
	struct xocl_gpio	*gpio;
	int			i, id, ret = 0;
	struct resource		*res;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->pdev = pdev;
	platform_set_drvdata(pdev, gpio);

	xocl_info(pdev, "probing...");
	for (i = 0, res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	    res;
	    res = platform_get_resource(pdev, IORESOURCE_MEM, ++i)) {
		id = xocl_gpio_name2id(gpio, res->name);
		if (id < 0) {
			xocl_err(pdev, "ep %s not found", res->name);
			continue;
		}
		gpio->base_addrs[id] = ioremap(res->start,
			res->end - res->start + 1);
		if (!gpio->base_addrs[id]) {
			xocl_err(pdev, "map base failed %pR", res);
			ret = -EIO;
			goto failed;
		}
		gpio->sizes[id] = res->end - res->start + 1;
	}

failed:
	if (ret)
		xocl_gpio_remove(pdev);

	return ret;
}

struct xocl_subdev_endpoints xocl_gpio_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			/* add name if ep is in same partition */
			{ .ep_name = NODE_BLP_ROM },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_GOLDEN_VER },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	/* adding ep bundle generates gpio device instance */
	{ 0 },
};

struct xocl_subdev_drvdata xocl_gpio_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_gpio_leaf_ioctl,
	},
};

static const struct platform_device_id xocl_gpio_table[] = {
	{ XOCL_GPIO, (kernel_ulong_t)&xocl_gpio_data },
	{ },
};

struct platform_driver xocl_gpio_driver = {
	.driver = {
		.name = XOCL_GPIO,
	},
	.probe = xocl_gpio_probe,
	.remove = xocl_gpio_remove,
	.id_table = xocl_gpio_table,
};
