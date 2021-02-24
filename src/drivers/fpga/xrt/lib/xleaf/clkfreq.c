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

#define OCL_CLKWIZ_STATUS_MASK		0xffff

#define OCL_CLKWIZ_STATUS_MEASURE_START	0x1
#define OCL_CLKWIZ_STATUS_MEASURE_DONE	0x2
#define OCL_CLK_FREQ_COUNTER_OFFSET	0x8
#define OCL_CLK_FREQ_V5_COUNTER_OFFSET	0x10
#define OCL_CLK_FREQ_V5_CLK0_ENABLED	0x10000

struct clkfreq {
	struct platform_device	*pdev;
	void __iomem		*clkfreq_base;
	const char		*clkfreq_ep_name;
	struct mutex		clkfreq_lock; /* clock counter dev lock */
};

static inline u32 reg_rd(struct clkfreq *clkfreq, u32 offset)
{
	return ioread32(clkfreq->clkfreq_base + offset);
}

static inline void reg_wr(struct clkfreq *clkfreq, u32 val, u32 offset)
{
	iowrite32(val, clkfreq->clkfreq_base + offset);
}

static u32 clkfreq_read(struct clkfreq *clkfreq)
{
	u32 freq = 0, status;
	int times = 10;

	mutex_lock(&clkfreq->clkfreq_lock);
	reg_wr(clkfreq, OCL_CLKWIZ_STATUS_MEASURE_START, 0);
	while (times != 0) {
		status = reg_rd(clkfreq, 0);
		if ((status & OCL_CLKWIZ_STATUS_MASK) ==
		    OCL_CLKWIZ_STATUS_MEASURE_DONE)
			break;
		mdelay(1);
		times--;
	};
	if (times > 0) {
		freq = (status & OCL_CLK_FREQ_V5_CLK0_ENABLED) ?
			reg_rd(clkfreq, OCL_CLK_FREQ_V5_COUNTER_OFFSET) :
			reg_rd(clkfreq, OCL_CLK_FREQ_COUNTER_OFFSET);
	}
	mutex_unlock(&clkfreq->clkfreq_lock);

	return freq;
}

static ssize_t freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct clkfreq *clkfreq = platform_get_drvdata(to_platform_device(dev));
	u32 freq;
	ssize_t count;

	freq = clkfreq_read(clkfreq);
	count = snprintf(buf, 64, "%d\n", freq);

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
xrt_clkfreq_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct clkfreq		*clkfreq;
	int			ret = 0;

	clkfreq = platform_get_drvdata(pdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_CLKFREQ_READ: {
		*(u32 *)arg = clkfreq_read(clkfreq);
		break;
	}
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int clkfreq_remove(struct platform_device *pdev)
{
	struct clkfreq *clkfreq;

	clkfreq = platform_get_drvdata(pdev);
	if (!clkfreq) {
		xrt_err(pdev, "driver data is NULL");
		return -EINVAL;
	}

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, clkfreq);

	CLKFREQ_INFO(clkfreq, "successfully removed clkfreq subdev");
	return 0;
}

static int clkfreq_probe(struct platform_device *pdev)
{
	struct clkfreq *clkfreq = NULL;
	struct resource *res;
	int ret;

	clkfreq = devm_kzalloc(&pdev->dev, sizeof(*clkfreq), GFP_KERNEL);
	if (!clkfreq)
		return -ENOMEM;

	platform_set_drvdata(pdev, clkfreq);
	clkfreq->pdev = pdev;
	mutex_init(&clkfreq->clkfreq_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	clkfreq->clkfreq_base = ioremap(res->start, res->end - res->start + 1);
	if (!clkfreq->clkfreq_base) {
		CLKFREQ_ERR(clkfreq, "map base %pR failed", res);
		ret = -EFAULT;
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
	clkfreq_remove(pdev);
	return ret;
}

static struct xrt_subdev_endpoints xrt_clkfreq_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .regmap_name = "freq_cnt" },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_clkfreq_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_clkfreq_leaf_ioctl,
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
