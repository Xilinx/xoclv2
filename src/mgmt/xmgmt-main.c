// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA MGMT PF entry point driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Sonal Santan <sonals@xilinx.com>
 */

#include <linux/firmware.h>
#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "uapi/xmgmt-ioctl.h"

#define	XMGMT_MAIN "xmgmt_main"

struct xmgmt_main {
	struct platform_device *pdev;
	void *evt_hdl;
	struct mutex busy_mutex;
	char *firmware;
};

static bool xmgmt_main_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	return id == XOCL_SUBDEV_QSPI;
}

static ssize_t reset_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);

	xocl_subdev_broadcast_event(pdev, XOCL_EVENT_PRE_HOT_RESET);
	(void) xocl_subdev_hot_reset(pdev);
	xocl_subdev_broadcast_event(pdev, XOCL_EVENT_POST_HOT_RESET);
	return count;
}
static DEVICE_ATTR_WO(reset);

static struct attribute *xmgmt_main_attrs[] = {
	&dev_attr_reset.attr,
	NULL,
};

static const struct attribute_group xmgmt_main_attrgroup = {
	.attrs = xmgmt_main_attrs,
};

static int xmgmt_main_event_cb(struct platform_device *pdev,
	enum xocl_events evt, enum xocl_subdev_id id, int instance)
{
	xocl_info(pdev, "event %d for (%d, %d)", evt, id, instance);

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		break;
	default:
		return 0;
	}

	return 0;
}

static int load_firmware_from_disk(struct platform_device *pdev, char **fw_buf)
{
	const char *uuid = "f465b0a3ae8c64f619bc150384ace69b"; // HACK FOR NOW
	int err = 0;
	char fw_name[256];
	const struct firmware *fw;

	snprintf(fw_name, sizeof(fw_name), "xilinx/%s/partition.xsabin", uuid);

	xocl_info(pdev, "try loading fw: %s", fw_name);

	err = request_firmware(&fw, fw_name, DEV(pdev));
	if (err)
		return err;

	*fw_buf = vmalloc(fw->size);
	if (*fw_buf != NULL)
		memcpy(*fw_buf, fw->data, fw->size);
	else
		err = -ENOMEM;

	release_firmware(fw);
	return 0;
}

static int xmgmt_main_probe(struct platform_device *pdev)
{
	struct xmgmt_main *xmm;
	int rc = 0;

	xocl_info(pdev, "probing...");

	xmm = devm_kzalloc(DEV(pdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->pdev = pdev;
	platform_set_drvdata(pdev, xmm);
	mutex_init(&xmm->busy_mutex);

	rc = load_firmware_from_disk(pdev, &xmm->firmware);
	if (rc == -ENOENT) {
		/*
		 * Can't find firmware on disk, waiting for flash driver
		 * to be online so that we can try to load it from flash.
		 */
		xmm->evt_hdl = xocl_subdev_add_event_cb(pdev,
			xmgmt_main_leaf_match, NULL, xmgmt_main_event_cb);
	}

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xmgmt_main_attrgroup))
		xocl_err(pdev, "failed to create sysfs group");

	return 0;
}

static int xmgmt_main_remove(struct platform_device *pdev)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);

	/* By now, partition driver should prevent any inter-leaf call. */

	xocl_info(pdev, "leaving...");

	if (xmm->evt_hdl)
		(void) xocl_subdev_remove_event_cb(pdev, xmm->evt_hdl);
	vfree(xmm->firmware);
	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xmgmt_main_attrgroup);
	return 0;
}

static int
xmgmt_main_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	xocl_info(pdev, "handling IOCTL cmd: %d", cmd);
	return 0;
}

static int xmgmt_main_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xocl_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xocl_info(pdev, "opened");
	file->private_data = platform_get_drvdata(pdev);
	return 0;
}

static int xmgmt_main_close(struct inode *inode, struct file *file)
{
	struct xmgmt_main *xmm = file->private_data;

	xocl_devnode_close(inode);

	xocl_info(xmm->pdev, "closed");
	return 0;
}

static long xmgmt_main_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long result = 0;
	struct xmgmt_main *xmm = filp->private_data;

	BUG_ON(!xmm);

	if (_IOC_TYPE(cmd) != XCLMGMT_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&xmm->busy_mutex);

	xocl_info(xmm->pdev, "ioctl cmd %d, arg %ld", cmd, arg);
	switch (cmd) {
	case XCLMGMT_IOCICAPDOWNLOAD_AXLF:
		break;
	case XCLMGMT_IOCFREQSCALE:
		break;
	default:
		result = -ENOTTY;
	}

	mutex_unlock(&xmm->busy_mutex);
	return result;
}

struct xocl_subdev_endpoints xocl_mgmt_main_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names []){
			{ .ep_name = NODE_MGMT_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xocl_subdev_drvdata xmgmt_main_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xmgmt_main_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xmgmt_main_open,
			.release = xmgmt_main_close,
			.unlocked_ioctl = xmgmt_main_ioctl,
		},
		.xsf_dev_name = "xmgmt",
	},
};

static const struct platform_device_id xmgmt_main_id_table[] = {
	{ XMGMT_MAIN, (kernel_ulong_t)&xmgmt_main_data },
	{ },
};

struct platform_driver xmgmt_main_driver = {
	.driver	= {
		.name    = XMGMT_MAIN,
	},
	.probe   = xmgmt_main_probe,
	.remove  = xmgmt_main_remove,
	.id_table = xmgmt_main_id_table,
};
