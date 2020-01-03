/*
 * A GEM style device manager for PCIe based OpenCL accelerators.
 *
 * Copyright (C) 2016-2018 Xilinx, Inc. All rights reserved.
 *
 * Authors:
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/pci.h>
#include <linux/platform_device.h>
#include "xocl-devices.h"
#include "xocl-features.h"

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "%s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xocl_subdev_ops rom_ops = {
	.ioctl = myioctl,
};

static int xocl_rom_probe(struct platform_device *pdev)
{
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	int ret;

	xocl_info(dev, "Probed %s/%s: Info 0x%px Subdev 0x%px\n", info->name, pdev->name,
		   info, pdev);

//	platform_set_drvdata(pdev, (void *)&rom_ops);
	return 0;

eprobe_mgr_put:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xocl_rom_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	xocl_info(dev, "Removed %s/%s\n", info->name, pdev->name);
	return 0;
}

static const struct platform_device_id rom_id_table[] = {
	{ "xocl-rom", (kernel_ulong_t)&rom_ops },
	{ },
};

struct platform_driver xocl_rom_driver = {
	.driver	= {
		.name    = "xocl-rom",
	},
	.probe    = xocl_rom_probe,
	.remove   = xocl_rom_remove,
	.id_table = rom_id_table,
};
