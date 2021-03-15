// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA memory calibration driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * memory calibration
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */
#include <linux/delay.h>
#include <linux/regmap.h>
#include "xclbin-helper.h"
#include "metadata.h"
#include "xleaf/ddr-srsr.h"
#include "xleaf/ddr_calibration.h"

#define XRT_CALIB	"xrt_calib"

#define XRT_CALIB_STATUS_REG		0
#define XRT_CALIB_READ_RETRIES		20
#define XRT_CALIB_READ_INTERVAL		500 /* ms */

static const struct regmap_config calib_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x1000,
};

struct calib_cache {
	struct list_head	link;
	const char		*ep_name;
	char			*data;
	u32			data_size;
};

struct calib {
	struct platform_device	*pdev;
	struct regmap		*regmap;
	struct mutex		lock; /* calibration dev lock */
	struct list_head	cache_list;
	u32			cache_num;
	enum xrt_calib_results	result;
};

static void __calib_cache_clean_nolock(struct calib *calib)
{
	struct calib_cache *cache, *temp;

	list_for_each_entry_safe(cache, temp, &calib->cache_list, link) {
		vfree(cache->data);
		list_del(&cache->link);
		vfree(cache);
	}
	calib->cache_num = 0;
}

static void calib_cache_clean(struct calib *calib)
{
	mutex_lock(&calib->lock);
	__calib_cache_clean_nolock(calib);
	mutex_unlock(&calib->lock);
}

static int calib_srsr(struct calib *calib, struct platform_device *srsr_leaf)
{
	const char		*ep_name;
	int			ret;
	struct calib_cache	*cache = NULL, *temp;
	struct xrt_srsr_calib req = { 0 };

	ret = xleaf_call(srsr_leaf, XRT_SRSR_EP_NAME, (void *)&ep_name);
	if (ret) {
		xrt_err(calib->pdev, "failed to get SRSR name %d", ret);
		goto done;
	}
	xrt_info(calib->pdev, "Calibrate SRSR %s", ep_name);

	mutex_lock(&calib->lock);
	list_for_each_entry_safe(cache, temp, &calib->cache_list, link) {
		if (!strncmp(ep_name, cache->ep_name, strlen(ep_name) + 1)) {
			req.xsic_buf = cache->data;
			req.xsic_size = cache->data_size;
			ret = xleaf_call(srsr_leaf, XRT_SRSR_FAST_CALIB, &req);
			if (ret) {
				xrt_err(calib->pdev, "Fast calib failed %d", ret);
				break;
			}
			goto done;
		}
	}

	if (ret) {
		/* fall back to full calibration */
		xrt_info(calib->pdev, "fall back to full calibration");
		vfree(cache->data);
		memset(cache, 0, sizeof(*cache));
	} else {
		/* First full calibration */
		cache = vzalloc(sizeof(*cache));
		if (!cache) {
			ret = -ENOMEM;
			goto done;
		}
		list_add(&cache->link, &calib->cache_list);
		calib->cache_num++;
	}

	req.xsic_buf = &cache->data;
	ret = xleaf_call(srsr_leaf, XRT_SRSR_CALIB, &req);
	if (ret) {
		xrt_err(calib->pdev, "Full calib failed %d", ret);
		list_del(&cache->link);
		calib->cache_num--;
		goto done;
	}
	cache->data_size = req.xsic_size;

done:
	mutex_unlock(&calib->lock);

	if (ret && cache) {
		vfree(cache->data);
		vfree(cache);
	}
	return ret;
}

static int calib_calibration(struct calib *calib)
{
	u32 times = XRT_CALIB_READ_RETRIES;
	u32 status;
	int ret;

	while (times != 0) {
		ret = regmap_read(calib->regmap, XRT_CALIB_STATUS_REG, &status);
		if (ret) {
			xrt_err(calib->pdev, "failed to read status reg %d", ret);
			return ret;
		}

		if (status & BIT(0))
			break;
		msleep(XRT_CALIB_READ_INTERVAL);
		times--;
	}

	if (!times) {
		xrt_err(calib->pdev,
			"MIG calibration timeout after bitstream download");
		return -ETIMEDOUT;
	}

	xrt_info(calib->pdev, "took %dms", (XRT_CALIB_READ_RETRIES - times) *
		 XRT_CALIB_READ_INTERVAL);
	return 0;
}

static void xrt_calib_event_cb(struct platform_device *pdev, void *arg)
{
	struct calib *calib = platform_get_drvdata(pdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	struct platform_device *leaf;
	enum xrt_subdev_id id;
	int ret, instance;

	id = evt->xe_subdev.xevt_subdev_id;
	instance = evt->xe_subdev.xevt_subdev_instance;

	switch (e) {
	case XRT_EVENT_POST_CREATION:
		if (id == XRT_SUBDEV_SRSR) {
			leaf = xleaf_get_leaf_by_id(pdev,
						    XRT_SUBDEV_SRSR,
						    instance);
			if (!leaf) {
				xrt_err(pdev, "does not get SRSR subdev");
				return;
			}
			ret = calib_srsr(calib, leaf);
			xleaf_put_leaf(pdev, leaf);
		} else if (id == XRT_SUBDEV_UCS) {
			ret = calib_calibration(calib);
		}
		if (ret)
			calib->result = XRT_CALIB_FAILED;
		else
			calib->result = XRT_CALIB_SUCCEEDED;
		break;
	default:
		xrt_dbg(pdev, "ignored event %d", e);
		break;
	}
}

static int xrt_calib_remove(struct platform_device *pdev)
{
	struct calib *calib = platform_get_drvdata(pdev);

	calib_cache_clean(calib);

	return 0;
}

static int xrt_calib_probe(struct platform_device *pdev)
{
	void __iomem *base = NULL;
	struct resource *res;
	struct calib *calib;
	int err = 0;

	calib = devm_kzalloc(&pdev->dev, sizeof(*calib), GFP_KERNEL);
	if (!calib)
		return -ENOMEM;

	calib->pdev = pdev;
	platform_set_drvdata(pdev, calib);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		err = -EINVAL;
		goto failed;
	}

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base)) {
		err = PTR_ERR(base);
		goto failed;
	}

	calib->regmap = devm_regmap_init_mmio(&pdev->dev, base, &calib_regmap_config);
	if (IS_ERR(calib->regmap)) {
		xrt_err(pdev, "Map iomem failed");
		err = PTR_ERR(calib->regmap);
		goto failed;
	}

	mutex_init(&calib->lock);
	INIT_LIST_HEAD(&calib->cache_list);

	return 0;

failed:
	return err;
}

static int
xrt_calib_leaf_call(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct calib *calib = platform_get_drvdata(pdev);
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xrt_calib_event_cb(pdev, arg);
		break;
	case XRT_CALIB_RESULT: {
		enum xrt_calib_results *r = (enum xrt_calib_results *)arg;
		*r = calib->result;
		break;
	}
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		ret = -EINVAL;
	}
	return ret;
}

static struct xrt_subdev_endpoints xrt_calib_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_DDR_CALIB },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_calib_data = {
	.xsd_dev_ops = {
		.xsd_leaf_call = xrt_calib_leaf_call,
	},
};

static const struct platform_device_id xrt_calib_table[] = {
	{ XRT_CALIB, (kernel_ulong_t)&xrt_calib_data },
	{ },
};

static struct platform_driver xrt_calib_driver = {
	.driver = {
		.name = XRT_CALIB,
	},
	.probe = xrt_calib_probe,
	.remove = xrt_calib_remove,
	.id_table = xrt_calib_table,
};

void calib_leaf_init_fini(bool init)
{
	if (init)
		xleaf_register_driver(XRT_SUBDEV_CALIB, &xrt_calib_driver, xrt_calib_endpoints);
	else
		xleaf_unregister_driver(XRT_SUBDEV_CALIB);
}
