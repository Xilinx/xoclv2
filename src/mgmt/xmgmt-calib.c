// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */
#include "xocl-xclbin.h"
#include "xocl-metadata.h"
#include "xocl-ddr-srsr.h"

#define CALIB_MAX_DDR_NUM	8

struct calib_cache {
	uint64_t	mem_id;
	char		*data;
	uint32_t	cache_size;
};

struct calib {
	struct platform_device	*main_pdev;
	struct platform_device	*srsr_pdev[CALIB_MAX_DDR_NUM];
	struct mutex		lock;
	struct calib_cache	cache[CALIB_MAX_DDR_NUM];
	uint32_t		cache_num;
};
#define calib_info(calib, fmt, arg...)			\
	xocl_info(calib->main_pdev, "calib" fmt, ##arg)
#define calib_err(calib, fmt, arg...)			\
	xocl_err(calib->main_pdev, "calib" fmt, ##arg)
#define calib_dbg(calib, fmt, arg...)			\
	xocl_dbg(calib->main_pdev, "calib" fmt, ##arg)

static int calib_save_by_idx(struct calib *calib, uint32_t idx)
{
	int err = 0;
	uint32_t cache_size = 0;
	struct xocl_srsr_ioctl_rw rd_arg;

	BUG_ON(!calib->cache);

	if (calib->cache[idx].data) {
		calib_info(calib, "Already have bank %d calib data, skip", idx);
		return 0;
	}

	err = xocl_subdev_ioctl(calib->srsr_pdev[idx],
		XOCL_DDR_SRSR_SIZE, &cache_size);

	if (err) {
		calib_err(calib, "get size failed %d", err);
		goto done;
	}

	calib->cache[idx].cache_size = cache_size;

	calib->cache[idx].data = vzalloc(cache_size);
	if (!calib->cache[idx].data) {
		err = -ENOMEM;
		goto done;
	}

	rd_arg.xdirw_buf = calib->cache[idx].data;
	rd_arg.xdirw_size = cache_size;
	err = xocl_subdev_ioctl(calib->srsr_pdev[idx],
		XOCL_DDR_SRSR_READ, &rd_arg);

done:
	if (err) {
		vfree(calib->cache[idx].data);
		calib->cache[idx].data = NULL;
	}
	return err;
}

static void calib_cache_clean(struct calib *calib)
{
	int i = 0;

	mutex_lock(&calib->lock);

	for (; i < calib->cache_num; ++i) {
		vfree(calib->cache[i].data);
		calib->cache[i].data = NULL;
	}
	mutex_unlock(&calib->lock);
}

static int calib_save(struct calib *calib)
{
	int err = 0;
	int i = 0;

	mutex_lock(&calib->lock);

	for (; i < calib->cache_num; ++i) {
		err = calib_save_by_idx(calib, i);
		if (err) {
			calib_err(calib, "save ddr %d failed %d", i, err);
			break;
		}
	}

	mutex_unlock(&calib->lock);

	if (err) {
		for (i--; i >=0; i--)
			calib_cache_clean(calib);
	}
	return err;
}

static int calib_restore(struct calib *calib)
{
	int err = 0;
	uint32_t i = 0;
	struct xocl_srsr_ioctl_rw wr_arg;

	mutex_lock(&calib->lock);

	for (; i < calib->cache_num; ++i) {
		if (!calib->cache[i].data)
			continue;

		wr_arg.xdirw_buf = calib->cache[i].data;
		wr_arg.xdirw_size = calib->cache[i].cache_size;
		err = xocl_subdev_ioctl(calib->srsr_pdev[i],
			XOCL_DDR_SRSR_WRITE, &wr_arg);
		if (err)
			calib_err(calib, "restore ddr %d failed %d", i, err);
	}

	mutex_unlock(&calib->lock);
	return err;
}


int calib_create(struct platform_device *pdev, void **calib_inst)
{
	struct calib *calib;

	calib = devm_kzalloc(&pdev->dev, sizeof(*calib), GFP_KERNEL);
	if (!calib)
		return -ENOMEM;

	calib->main_pdev = pdev;

	mutex_init(&calib->lock);
	*calib_inst = calib;

	return 0;
}


void calib_destroy(struct platform_device *pdev, void *calib_inst)
{
	struct calib *calib = calib_inst;

	calib_cache_clean(calib);

	devm_kfree(&pdev->dev, calib);
}
