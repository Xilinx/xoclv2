// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Clock Frequency Counter Driver
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
#include "xleaf/clkfreq.h"

#define CLKFREQ_ERR(clkfreq, fmt, arg...)   \
	xrt_err((clkfreq)->pdev, fmt "\n", ##arg)
#define CLKFREQ_WARN(clkfreq, fmt, arg...)  \
	xrt_warn((clkfreq)->pdev, fmt "\n", ##arg)
#define CLKFREQ_INFO(clkfreq, fmt, arg...)  \
	xrt_info((clkfreq)->pdev, fmt "\n", ##arg)
#define CLKFREQ_DBG(clkfreq, fmt, arg...)   \
	xrt_dbg((clkfreq)->pdev, fmt "\n", ##arg)

#define XRT_CLKFREQ		"xrt_clkfreq"

#define XRT_CLKFREQ_CONTROL_STATUS_MASK		0xffff

#define XRT_CLKFREQ_CONTROL_START	0x1
#define XRT_CLKFREQ_CONTROL_DONE	0x2
#define XRT_CLKFREQ_V5_CLK0_ENABLED	0x10000

#define XRT_CLKFREQ_CONTROL_REG		0
#define XRT_CLKFREQ_COUNT_REG		0x8
#define XRT_CLKFREQ_V5_COUNT_REG	0x10

#define XRT_CLKFREQ_READ_RETRIES	10

static const struct regmap_config clkfreq_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x1000,
};

struct clkfreq {
	struct platform_device	*pdev;
	struct regmap		*regmap;
	const char		*clkfreq_ep_name;
	struct mutex		clkfreq_lock; /* clock counter dev lock */
};

static int clkfreq_read(struct clkfreq *clkfreq, u32 *freq)
{
	int times = XRT_CLKFREQ_READ_RETRIES;
	u32 status;
	int ret;

	*freq = 0;
	mutex_lock(&clkfreq->clkfreq_lock);
	ret = regmap_write(clkfreq->regmap, XRT_CLKFREQ_CONTROL_REG, XRT_CLKFREQ_CONTROL_START);
	if (ret) {
		CLKFREQ_INFO(clkfreq, "write start to control reg failed %d", ret);
		goto failed;
	}
	while (times != 0) {
		ret = regmap_read(clkfreq->regmap, XRT_CLKFREQ_CONTROL_REG, &status);
		if (ret) {
			CLKFREQ_INFO(clkfreq, "read control reg failed %d", ret);
			goto failed;
		}
		if ((status & XRT_CLKFREQ_CONTROL_STATUS_MASK) == XRT_CLKFREQ_CONTROL_DONE)
			break;
		mdelay(1);
		times--;
	};

	if (!times) {
		ret = -ETIMEDOUT;
		goto failed;
	}

	if (status & XRT_CLKFREQ_V5_CLK0_ENABLED)
		ret = regmap_read(clkfreq->regmap, XRT_CLKFREQ_V5_COUNT_REG, freq);
	else
		ret = regmap_read(clkfreq->regmap, XRT_CLKFREQ_COUNT_REG, freq);
	if (ret) {
		CLKFREQ_INFO(clkfreq, "read count failed %d", ret);
		goto failed;
	}

	mutex_unlock(&clkfreq->clkfreq_lock);

	return 0;

failed:
	mutex_unlock(&clkfreq->clkfreq_lock);

	return ret;
}

static ssize_t freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct clkfreq *clkfreq = platform_get_drvdata(to_platform_device(dev));
	ssize_t count;
	u32 freq;

	if (clkfreq_read(clkfreq, &freq))
		return -EINVAL;

	count = snprintf(buf, 64, "%u\n", freq);

	return count;
}
static DEVICE_ATTR_RO(freq);

static struct attribute *clkfreq_attrs[] = {
	&dev_attr_freq.attr,
	NULL,
};

static struct attribute_group clkfreq_attr_group = {
	.attrs = clkfreq_attrs,
};

static int
xrt_clkfreq_leaf_call(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct clkfreq *clkfreq;
	int ret = 0;

	clkfreq = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_CLKFREQ_READ:
		ret = clkfreq_read(clkfreq, arg);
		break;
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int clkfreq_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &clkfreq_attr_group);

	return 0;
}

static int clkfreq_probe(struct platform_device *pdev)
{
	struct clkfreq *clkfreq = NULL;
	void __iomem *base = NULL;
	struct resource *res;
	int ret;

	clkfreq = devm_kzalloc(&pdev->dev, sizeof(*clkfreq), GFP_KERNEL);
	if (!clkfreq)
		return -ENOMEM;

	platform_set_drvdata(pdev, clkfreq);
	clkfreq->pdev = pdev;
	mutex_init(&clkfreq->clkfreq_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -EINVAL;
		goto failed;
	}
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto failed;
	}

	clkfreq->regmap = devm_regmap_init_mmio(&pdev->dev, base, &clkfreq_regmap_config);
	if (IS_ERR(clkfreq->regmap)) {
		CLKFREQ_ERR(clkfreq, "regmap %pR failed", res);
		ret = PTR_ERR(clkfreq->regmap);
		goto failed;
	}
	clkfreq->clkfreq_ep_name = res->name;

	ret = sysfs_create_group(&pdev->dev.kobj, &clkfreq_attr_group);
	if (ret) {
		CLKFREQ_ERR(clkfreq, "create clkfreq attrs failed: %d", ret);
		goto failed;
	}

	CLKFREQ_INFO(clkfreq, "successfully initialized clkfreq subdev");

	return 0;

failed:
	return ret;
}

static struct xrt_subdev_endpoints xrt_clkfreq_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .regmap_name = XRT_MD_REGMAP_CLKFREQ },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_clkfreq_data = {
	.xsd_dev_ops = {
		.xsd_leaf_call = xrt_clkfreq_leaf_call,
	},
};

static const struct platform_device_id xrt_clkfreq_table[] = {
	{ XRT_CLKFREQ, (kernel_ulong_t)&xrt_clkfreq_data },
	{ },
};

static struct platform_driver xrt_clkfreq_driver = {
	.driver = {
		.name = XRT_CLKFREQ,
	},
	.probe = clkfreq_probe,
	.remove = clkfreq_remove,
	.id_table = xrt_clkfreq_table,
};

void clkfreq_leaf_init_fini(bool init)
{
	if (init) {
		xleaf_register_driver(XRT_SUBDEV_CLKFREQ,
				      &xrt_clkfreq_driver, xrt_clkfreq_endpoints);
	} else {
		xleaf_unregister_driver(XRT_SUBDEV_CLKFREQ);
	}
}
