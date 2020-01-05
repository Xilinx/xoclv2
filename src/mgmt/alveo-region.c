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

#include "xmgmt-drv.h"
#include "xocl-devices.h"

static int xmgmt_region_get_bridges(struct fpga_region *region)
{
	return 0;
}

static inline bool is_fixed_region(const struct xocl_region *part)
{
	return ((part->id == XOCL_REGION_STATIC) || (part->id == XOCL_REGION_BLD));
}

static int xmgmt_region_probe(struct platform_device *pdev)
{
	struct xocl_region *part = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct fpga_region *region;
	struct fpga_manager *mgr = NULL;
	const char *kind = "Static";
	int ret;

	BUG_ON(part->region != pdev);
	/* No FPGA manager for static regions */
	if (!is_fixed_region(part)) {
		kind = "Dynamic";
		mgr = fpga_mgr_get(&part->lro->fmgr->dev);
		if (IS_ERR(mgr))
			return -EPROBE_DEFER;
	}
	xmgmt_info(dev, "%s Part 0x%px ID %x\n", kind, part, part->id);
	xmgmt_info(dev, "FPGA Manager 0x%px\n", mgr);
	region = devm_fpga_region_create(dev, mgr, xmgmt_region_get_bridges);
	if (!region) {
		ret = -ENOMEM;
		goto eprobe_mgr_put;
	}
	xmgmt_info(dev, "Allocated FPGA Region 0x%px\n", region);

	region->priv = part;
	region->compat_id = mgr ? mgr->compat_id : NULL;
	platform_set_drvdata(pdev, region);
	ret = fpga_region_register(region);
	if (ret)
		goto eprobe_mgr_put;
	xmgmt_info(dev, "Alveo FPGA Region ID %x probed\n", part->id);

	return 0;

eprobe_mgr_put:
	if (mgr) fpga_mgr_put(mgr);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xmgmt_region_remove(struct platform_device *pdev)
{
	struct xmgmt_region *part = dev_get_platdata(&pdev->dev);
	struct fpga_region *region = platform_get_drvdata(pdev);
	struct fpga_manager *mgr = region->mgr;
	struct device *dev = &pdev->dev;

	xmgmt_info(dev, "Remove FPGA Region 0x%px\n", region);
	fpga_region_unregister(region);
	if (mgr) fpga_mgr_put(mgr);

	return 0;
}

static struct platform_driver xmgmt_region_driver = {
	.driver	= {
		.name    = "xocl-region",
	},
	.probe   = xmgmt_region_probe,
	.remove  = xmgmt_region_remove,
};

module_platform_driver(xmgmt_region_driver);

MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo FPGA Region driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:xocl-region");
