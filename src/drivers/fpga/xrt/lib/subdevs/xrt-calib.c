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
#include "xrt-xclbin.h"
#include "metadata.h"
#include "subdev/calib.h"

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
	void			*evt_hdl;
	enum xrt_calib_results	result;
};

#define CALIB_DONE(calib)			\
	(ioread32(calib->calib_base) & BIT(0))

static bool xrt_calib_leaf_match(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	if (id == XRT_SUBDEV_UCS || id == XRT_SUBDEV_SRSR)
		return true;

	return false;
}

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
	return -ENOTSUPP;
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

static int xrt_calib_event_cb(struct platform_device *pdev,
	enum xrt_events evt, void *arg)
{
	struct calib *calib = platform_get_drvdata(pdev);
	struct xrt_event_arg_subdev *esd = (struct xrt_event_arg_subdev *)arg;
	struct platform_device *leaf;
	int ret;

	switch (evt) {
	case XRT_EVENT_POST_CREATION: {
		if (esd->xevt_subdev_id == XRT_SUBDEV_SRSR) {
			leaf = xrt_subdev_get_leaf_by_id(pdev,
				XRT_SUBDEV_SRSR, esd->xevt_subdev_instance);
			BUG_ON(!leaf);
			ret = calib_srsr(calib, leaf);
			xrt_subdev_put_leaf(pdev, leaf);
			calib->result =
				ret ? XRT_CALIB_FAILED : XRT_CALIB_SUCCEEDED;
		} else if (esd->xevt_subdev_id == XRT_SUBDEV_UCS) {
			ret = calib_calibration(calib);
			calib->result =
				ret ? XRT_CALIB_FAILED : XRT_CALIB_SUCCEEDED;
		}
		break;
	}
	default:
		xrt_info(pdev, "ignored event %d", evt);
		break;
	}

	return XRT_EVENT_CB_CONTINUE;
}

int xrt_calib_remove(struct platform_device *pdev)
{
	struct calib *calib = platform_get_drvdata(pdev);

	xrt_subdev_remove_event_cb(pdev, calib->evt_hdl);
	calib_cache_clean(calib);

	if (calib->calib_base)
		iounmap(calib->calib_base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, calib);

	return 0;
}

int xrt_calib_probe(struct platform_device *pdev)
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

	calib->evt_hdl = xrt_subdev_add_event_cb(pdev, xrt_calib_leaf_match,
		NULL, xrt_calib_event_cb);

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
