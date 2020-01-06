// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Xilinx acclerator XMC IP.
 * Bulk of the code borrowed from XRT mgmt driver file xmc.c
 *
 * Copyright (C) 2016-2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 *          chien-wei.lan@xilinx.com
 */

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include "xocl-devices.h"
#include "xocl-features.h"
#include "xocl-mailbox-proto.h"

static const struct file_operations xmc_fops;

static long myxmc_ioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "%s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

const static struct xocl_subdev_ops myxmc_ops = {
	.ioctl = myxmc_ioctl,
#if PF == MGMTPF
	.fops = &xmc_fops,
#endif
	.dev = -1,
};

static int xocl_xmc_probe(struct platform_device *pdev)
{
	int ret;
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct feature_rom *rom = devm_kzalloc(&pdev->dev, sizeof(*rom), GFP_KERNEL);
	if (!rom)
		return -ENOMEM;
	rom->pdev =  pdev;

	ret = feature_rom_probe_helper(pdev, res, rom);
	if (ret)
		goto out;
	platform_set_drvdata(pdev, rom);
	xocl_info(dev, "Probed subdev %s: resource %pr mapped @%px\n", pdev->name, res, rom->base);
	return 0;

out:
	devm_kfree(&pdev->dev, rom);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int xocl_xmc_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct feature_rom *rom = platform_get_drvdata(pdev);
	struct xocl_subdev_info *info = dev_get_platdata(&pdev->dev);

	if (rom->base)
		iounmap(rom->base);
	sysfs_remove_group(&pdev->dev.kobj, &rom_attr_group);
	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, rom);
	xocl_info(dev, "Removed subdev %s\n", pdev->name);
	return 0;
}

static const struct platform_device_id xmc_id_table[] = {
	{ "xocl-xmc", (kernel_ulong_t)&myxmc_ops },
	{ },
};

struct platform_driver xocl_xmc_driver = {
	.driver	= {
		.name    = "xocl-xmc",
	},
	.probe    = xocl_xmc_probe,
	.remove   = xocl_xmc_remove,
	.id_table = xmc_id_table,
};
