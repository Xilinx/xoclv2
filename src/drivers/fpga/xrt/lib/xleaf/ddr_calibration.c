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

XRT_DEFINE_REGMAP_CONFIG(calib_regmap_config);

struct calib_cache {
	struct list_head	link;
	const char		*ep_name;
	char			*data;
	u32			data_size;
};

struct calib {
	struct xrt_device	*xdev;
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

static int calib_srsr(struct calib *calib, struct xrt_device *srsr_leaf)
{
	const char		*ep_name;
	int			ret;
	struct calib_cache	*cache = NULL, *temp;
	struct xrt_srsr_calib req = { 0 };

	ret = xleaf_call(srsr_leaf, XRT_SRSR_EP_NAME, (void *)&ep_name);
	if (ret) {
		xrt_err(calib->xdev, "failed to get SRSR name %d", ret);
		goto done;
	}
	xrt_info(calib->xdev, "Calibrate SRSR %s", ep_name);

	mutex_lock(&calib->lock);
	list_for_each_entry_safe(cache, temp, &calib->cache_list, link) {
		if (!strncmp(ep_name, cache->ep_name, strlen(ep_name) + 1)) {
			req.xsic_buf = cache->data;
			req.xsic_size = cache->data_size;
			ret = xleaf_call(srsr_leaf, XRT_SRSR_FAST_CALIB, &req);
			if (ret) {
				xrt_err(calib->xdev, "Fast calib failed %d", ret);
				break;
			}
			goto done;
		}
	}

	if (ret) {
		/* fall back to full calibration */
		xrt_info(calib->xdev, "fall back to full calibration");
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
		xrt_err(calib->xdev, "Full calib failed %d", ret);
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
			xrt_err(calib->xdev, "failed to read status reg %d", ret);
			return ret;
		}

		if (status & BIT(0))
			break;
		msleep(XRT_CALIB_READ_INTERVAL);
		times--;
	}

	if (!times) {
		xrt_err(calib->xdev,
			"MIG calibration timeout after bitstream download");
		return -ETIMEDOUT;
	}

	xrt_info(calib->xdev, "took %dms", (XRT_CALIB_READ_RETRIES - times) *
		 XRT_CALIB_READ_INTERVAL);
	return 0;
}

static void xrt_calib_event_cb(struct xrt_device *xdev, void *arg)
{
	struct calib *calib = xrt_get_drvdata(xdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	struct xrt_device *leaf;
	enum xrt_subdev_id id;
	int ret, instance;

	id = evt->xe_subdev.xevt_subdev_id;
	instance = evt->xe_subdev.xevt_subdev_instance;

	switch (e) {
	case XRT_EVENT_POST_CREATION:
		if (id == XRT_SUBDEV_SRSR) {
			leaf = xleaf_get_leaf_by_id(xdev,
						    XRT_SUBDEV_SRSR,
						    instance);
			if (!leaf) {
				xrt_err(xdev, "does not get SRSR subdev");
				return;
			}
			ret = calib_srsr(calib, leaf);
			xleaf_put_leaf(xdev, leaf);
		} else if (id == XRT_SUBDEV_UCS) {
			ret = calib_calibration(calib);
		}
		if (ret)
			calib->result = XRT_CALIB_FAILED;
		else
			calib->result = XRT_CALIB_SUCCEEDED;
		break;
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		break;
	}
}

static void xrt_calib_remove(struct xrt_device *xdev)
{
	struct calib *calib = xrt_get_drvdata(xdev);

	calib_cache_clean(calib);
}

static int xrt_calib_probe(struct xrt_device *xdev)
{
	void __iomem *base = NULL;
	struct resource *res;
	struct calib *calib;
	int err = 0;

	calib = devm_kzalloc(&xdev->dev, sizeof(*calib), GFP_KERNEL);
	if (!calib)
		return -ENOMEM;

	calib->xdev = xdev;
	xrt_set_drvdata(xdev, calib);

	res = xrt_get_resource(xdev, IORESOURCE_MEM, 0);
	if (!res) {
		err = -EINVAL;
		goto failed;
	}

	base = devm_ioremap_resource(&xdev->dev, res);
	if (IS_ERR(base)) {
		err = PTR_ERR(base);
		goto failed;
	}

	calib->regmap = devm_regmap_init_mmio(&xdev->dev, base, &calib_regmap_config);
	if (IS_ERR(calib->regmap)) {
		xrt_err(xdev, "Map iomem failed");
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
xrt_calib_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct calib *calib = xrt_get_drvdata(xdev);
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xrt_calib_event_cb(xdev, arg);
		break;
	case XRT_CALIB_RESULT: {
		enum xrt_calib_results *r = (enum xrt_calib_results *)arg;
		*r = calib->result;
		break;
	}
	default:
		xrt_err(xdev, "unsupported cmd %d", cmd);
		ret = -EINVAL;
	}
	return ret;
}

static struct xrt_dev_endpoints xrt_calib_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_DDR_CALIB },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_driver xrt_calib_driver = {
	.driver = {
		.name = XRT_CALIB,
	},
	.subdev_id = XRT_SUBDEV_CALIB,
	.endpoints = xrt_calib_endpoints,
	.probe = xrt_calib_probe,
	.remove = xrt_calib_remove,
	.leaf_call = xrt_calib_leaf_call,
};

XRT_LEAF_INIT_FINI_FUNC(calib);
