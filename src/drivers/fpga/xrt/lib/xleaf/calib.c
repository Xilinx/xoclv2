// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA memory calibration driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * memory calibration
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */
#include <linux/delay.h>
#include "xclbin-helper.h"
#include "metadata.h"
#include "xleaf/ddr-srsr.h"
#include "xleaf/calib.h"

#define XRT_CALIB	"xrt_calib"

struct calib_cache {
	struct list_head	link;
	const char		*ep_name;
	char			*data;
	uint32_t		data_size;
};

struct calib {
	struct platform_device	*pdev;
	void			*calib_base;
	struct mutex		lock;
	struct list_head	cache_list;
	uint32_t		cache_num;
	enum xrt_calib_results	result;
};

#define CALIB_DONE(calib)			\
	(ioread32(calib->calib_base) & BIT(0))

static void calib_cache_clean_nolock(struct calib *calib)
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
	calib_cache_clean_nolock(calib);
	mutex_unlock(&calib->lock);
}

static int calib_srsr(struct calib *calib, struct platform_device *srsr_leaf)
{
	const char		*ep_name;
	int			ret;
	struct calib_cache	*cache = NULL, *temp;
	struct xrt_srsr_ioctl_calib req = { 0 };

	ret = xleaf_ioctl(srsr_leaf, XRT_SRSR_EP_NAME,
		(void *)&ep_name);
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
			ret = xleaf_ioctl(srsr_leaf,
				XRT_SRSR_FAST_CALIB, &req);
			if (ret) {
				xrt_err(calib->pdev, "Fast calib failed %d",
					ret);
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
	ret = xleaf_ioctl(srsr_leaf, XRT_SRSR_CALIB, &req);
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
	int i;

	for (i = 0; i < 20; i++) {
		if (CALIB_DONE(calib))
			break;
		msleep(500);
	}

	if (i == 20) {
		xrt_err(calib->pdev,
			"MIG calibration timeout after bitstream download");
		return -ETIMEDOUT;
	}

	xrt_info(calib->pdev, "took %dms", i * 500);
	return 0;
}

static void xrt_calib_event_cb(struct platform_device *pdev, void *arg)
{
	struct calib *calib = platform_get_drvdata(pdev);
		struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	enum xrt_subdev_id id = evt->xe_subdev.xevt_subdev_id;
	int instance = evt->xe_subdev.xevt_subdev_instance;
	struct platform_device *leaf;
	int ret;

	switch (e) {
	case XRT_EVENT_POST_CREATION: {
		if (id == XRT_SUBDEV_SRSR) {
			leaf = xleaf_get_leaf_by_id(pdev,
				XRT_SUBDEV_SRSR, instance);
			BUG_ON(!leaf);
			ret = calib_srsr(calib, leaf);
			xleaf_put_leaf(pdev, leaf);
			calib->result =
				ret ? XRT_CALIB_FAILED : XRT_CALIB_SUCCEEDED;
		} else if (id == XRT_SUBDEV_UCS) {
			ret = calib_calibration(calib);
			calib->result =
				ret ? XRT_CALIB_FAILED : XRT_CALIB_SUCCEEDED;
		}
		break;
	}
	default:
		break;
	}
}

static int xrt_calib_remove(struct platform_device *pdev)
{
	struct calib *calib = platform_get_drvdata(pdev);

	calib_cache_clean(calib);

	if (calib->calib_base)
		iounmap(calib->calib_base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, calib);

	return 0;
}

static int xrt_calib_probe(struct platform_device *pdev)
{
	struct calib *calib;
	struct resource *res;
	int err = 0;

	calib = devm_kzalloc(&pdev->dev, sizeof(*calib), GFP_KERNEL);
	if (!calib)
		return -ENOMEM;

	calib->pdev = pdev;
	platform_set_drvdata(pdev, calib);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		goto failed;

	calib->calib_base = ioremap(res->start, res->end - res->start + 1);
	if (!calib->calib_base) {
		err = -EIO;
		xrt_err(pdev, "Map iomem failed");
		goto failed;
	}

	mutex_init(&calib->lock);
	INIT_LIST_HEAD(&calib->cache_list);

	return 0;

failed:
	xrt_calib_remove(pdev);
	return err;
}

static int
xrt_calib_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
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

struct xrt_subdev_endpoints xrt_calib_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = NODE_DDR_CALIB },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xrt_subdev_drvdata xrt_calib_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_calib_leaf_ioctl,
	},
};

static const struct platform_device_id xrt_calib_table[] = {
	{ XRT_CALIB, (kernel_ulong_t)&xrt_calib_data },
	{ },
};

struct platform_driver xrt_calib_driver = {
	.driver = {
		.name = XRT_CALIB,
	},
	.probe = xrt_calib_probe,
	.remove = xrt_calib_remove,
	.id_table = xrt_calib_table,
};
