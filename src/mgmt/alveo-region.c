// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Region Driver
 *
 * Copyright (C) 2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#include <linux/module.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fpga/fpga-region.h>

#include "alveo-drv.h"

static int xmgmt_region_get_bridges(struct fpga_region *region)
{
	return 0;
}

static int xmgmt_region_probe(struct platform_device *pdev)
{
	void *pdata = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct fpga_region *region;
	struct fpga_manager *mgr;
	int ret;

	xmgmt_info(dev, "Region 0x%p 0x%p\n", pdata, dev);
/*
	mgr = fpga_mgr_get(&pdata->mgr->dev);
	if (IS_ERR(mgr))
		return -EPROBE_DEFER;

	region = devm_fpga_region_create(dev, mgr, xmgmt_region_get_bridges);
	if (!region) {
		ret = -ENOMEM;
		goto eprobe_mgr_put;
	}

	region->priv = pdata;
	region->compat_id = mgr->compat_id;
	platform_set_drvdata(pdev, region);

	ret = fpga_region_register(region);
	if (ret)
		goto eprobe_mgr_put;
*/
	xmgmt_info(dev, "Alveo FPGA Region probed\n");

	return 0;

eprobe_mgr_put:
	fpga_mgr_put(mgr);
	return ret;
}

static int xmgmt_region_remove(struct platform_device *pdev)
{
	struct fpga_region *region = platform_get_drvdata(pdev);
	struct fpga_manager *mgr = region->mgr;

	fpga_region_unregister(region);
//	fpga_mgr_put(mgr);

	return 0;
}

static struct platform_driver xmgmt_region_driver = {
	.driver	= {
		.name    = "alveo-region",
	},
	.probe   = xmgmt_region_probe,
	.remove  = xmgmt_region_remove,
};

module_platform_driver(xmgmt_region_driver);

MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo FPGA Region driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:alveo-region");
