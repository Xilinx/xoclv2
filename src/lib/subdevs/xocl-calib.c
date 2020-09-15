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
#include "xocl-xclbin.h"
#include "xocl-metadata.h"
#include "xocl-ddr-srsr.h"

#define XOCL_CALIB	"xocl_calib"

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
};

#define CALIB_DONE(calib)			\
	(ioread32(calib->calib_base) & BIT(0))

static bool xocl_calib_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	if (id == XOCL_SUBDEV_UCS || id == XOCL_SUBDEV_SRSR)
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
	mutex_unlock(&calib->lock);
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
	struct xocl_srsr_ioctl_calib req = { 0 };

	ret = xocl_subdev_ioctl(srsr_leaf, XOCL_SRSR_EP_NAME,
		(void *)&ep_name);
	if (ret) {
		xocl_err(calib->pdev, "failed to get SRSR name %d", ret);
		goto done;
	}
	xocl_info(calib->pdev, "Calibrate SRSR %s", ep_name);

	mutex_lock(&calib->lock);
	list_for_each_entry_safe(cache, temp, &calib->cache_list, link) {
		if (!strncmp(ep_name, cache->ep_name, strlen(ep_name) + 1)) {
			req.xsic_buf = cache->data;
			req.xsic_size = cache->data_size;
			ret = xocl_subdev_ioctl(srsr_leaf,
				XOCL_SRSR_FAST_CALIB, &req);
			if (ret) {
				xocl_err(calib->pdev, "Fast calib failed %d",
					ret);
				break;
			}
			goto done;
		}
	}

	if (ret) {
		/* fall back to full calibration */
		xocl_info(calib->pdev, "fall back to full calibration");
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
	ret = xocl_subdev_ioctl(srsr_leaf, XOCL_SRSR_CALIB, &req);
	if (ret) {
		xocl_err(calib->pdev, "Full calib failed %d", ret);
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
		xocl_err(calib->pdev,
			"MIG calibration timeout after bitstream download");
		return -ETIMEDOUT;
	}

	xocl_info(calib->pdev, "took %dms", i * 500);
	return 0;
}

static int xocl_calib_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct calib *calib = platform_get_drvdata(pdev);
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;
	struct platform_device *leaf;

	switch (evt) {
	case XOCL_EVENT_POST_CREATION: {
		if (esd->xevt_subdev_id == XOCL_SUBDEV_SRSR) {
			leaf = xocl_subdev_get_leaf_by_id(pdev,
				XOCL_SUBDEV_SRSR, esd->xevt_subdev_instance);
			BUG_ON(!leaf);
			calib_srsr(calib, leaf);
			xocl_subdev_put_leaf(pdev, leaf);
		} else if (esd->xevt_subdev_id == XOCL_SUBDEV_UCS)
			calib_calibration(calib);
		break;
	}
	default:
		xocl_info(pdev, "ignored event %d", evt);
		break;
	}

	return XOCL_EVENT_CB_CONTINUE;
}

int xocl_calib_remove(struct platform_device *pdev)
{
	struct calib *calib = platform_get_drvdata(pdev);

	xocl_subdev_remove_event_cb(pdev, calib->evt_hdl);
	calib_cache_clean(calib);

	if (calib->calib_base)
		iounmap(calib->calib_base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, calib);

	return 0;
}

int xocl_calib_probe(struct platform_device *pdev)
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
		xocl_err(pdev, "Map iomem failed");
		goto failed;
	}

	calib->evt_hdl = xocl_subdev_add_event_cb(pdev, xocl_calib_leaf_match,
		NULL, xocl_calib_event_cb);

	mutex_init(&calib->lock);
	INIT_LIST_HEAD(&calib->cache_list);

	return 0;

failed:
	xocl_calib_remove(pdev);
	return err;
}

struct xocl_subdev_endpoints xocl_calib_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_DDR_CALIB },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static const struct platform_device_id xocl_calib_table[] = {
	{ XOCL_CALIB, },
	{ },
};

struct platform_driver xocl_calib_driver = {
	.driver = {
		.name = XOCL_CALIB,
	},
	.probe = xocl_calib_probe,
	.remove = xocl_calib_remove,
	.id_table = xocl_calib_table,
};
