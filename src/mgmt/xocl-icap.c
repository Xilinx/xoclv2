// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Xilinx acclerator feature ROM IP.
 * Bulk of the code borrowed from XRT driver file feature_rom.c
 *
 * Copyright (C) 2016-2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 *          chien-wei.lan@xilinx.com
 */

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/cred.h>
#include "xocl-lib.h"
#include "xocl-features.h"
#include "xclbin.h"

static struct key *icap_keys = NULL;

struct xocl_icap {
	struct xocl_subdev_base  core;
};

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "Subdev %s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

static int xocl_post_init_icap(struct xocl_subdev_drv *ops)
{
	int err = 0;
	icap_keys = keyring_alloc(".xilinx_fpga_xclbin_keys", KUIDT_INIT(0),
				  KGIDT_INIT(0), current_cred(),
				  ((KEY_POS_ALL & ~KEY_POS_SETATTR) |
				   KEY_USR_VIEW | KEY_USR_WRITE | KEY_USR_SEARCH),
				  KEY_ALLOC_NOT_IN_QUOTA, NULL, NULL);

	if (IS_ERR(icap_keys)) {
		err = PTR_ERR(icap_keys);
		icap_keys = NULL;
		pr_err("create icap keyring failed: %d", err);
	}
	return err;
}

static void xocl_pre_exit_icap(struct xocl_subdev_drv *ops)
{
	if (icap_keys)
		key_put(icap_keys);
}

static int icap_open(struct inode *inode, struct file *file)
{
	struct xocl_icap *icap = container_of(inode->i_cdev, struct xocl_icap, core.chr_dev);

	if (!icap)
		return -ENXIO;

	file->private_data = icap;
	return 0;
}

static int icap_close(struct inode *inode, struct file *file)
{
	struct xocl_icap *icap = file->private_data;

	file->private_data = NULL;
	return 0;
}

static ssize_t icap_write_rp(struct file *filp, const char __user *data,
			     size_t data_len, loff_t *off)
{
	struct xocl_icap *icap = filp->private_data;
	struct axlf axlf_header = { {0} };
	struct axlf *axlf = NULL;
	const struct axlf_section_header *section;
	return 0;
}

static const struct file_operations icap_fops = {
	.open = icap_open,
	.release = icap_close,
	.write = icap_write_rp,
};

static struct xocl_subdev_drv irom_ops = {
	.ioctl = myioctl,
	.fops = &icap_fops,
	.id = XOCL_SUBDEV_ICAP,
	.dnum = -1,
	.subdrv_post_init = xocl_post_init_icap,
	.subdrv_pre_exit = xocl_pre_exit_icap,
};

static int xocl_icap_probe(struct platform_device *pdev)
{
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct xocl_icap *rom = devm_kzalloc(&pdev->dev, sizeof(struct xocl_icap), GFP_KERNEL);
	if (!rom)
		return -ENOMEM;
	rom->core.pdev =  pdev;
	platform_set_drvdata(pdev, rom);

	xocl_info(&pdev->dev, "Probed subdev %s: resource %pr", pdev->name, res);
	return 0;
}

static int xocl_icap_remove(struct platform_device *pdev)
{
	struct xocl_xmc *icap = platform_get_drvdata(pdev);
	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, icap);
	xocl_info(&pdev->dev, "Removed subdev %s\n", pdev->name);
	return 0;
}


static const struct platform_device_id icap_id_table[] = {
	{ XOCL_ICAP, (kernel_ulong_t)&irom_ops },
	{ },
};


struct platform_driver xocl_icap_driver = {
	.driver	= {
		.name    = XOCL_ICAP,
	},
	.probe    = xocl_icap_probe,
	.remove   = xocl_icap_remove,
	.id_table = icap_id_table,
};
