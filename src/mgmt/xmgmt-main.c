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
#include "xocl-xclbin.h"
#include "xocl-metadata.h"
#include "xocl-flash.h"
#include "xocl-subdev.h"
#include "uapi/flash_xrt_data.h"
#include "uapi/xmgmt-ioctl.h"
#include "xocl-gpio.h"

#define	XMGMT_MAIN "xmgmt_main"

struct xmgmt_main {
	struct platform_device *pdev;
	void *evt_hdl;
	struct mutex busy_mutex;
	char *firmware;
	bool flash_ready;
	bool gpio_ready;
};

static bool xmgmt_main_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	if (id == XOCL_SUBDEV_GPIO)
		return xocl_gpio_match_epname(id, pdev, arg);
	else if (id == XOCL_SUBDEV_QSPI)
		return true;

	return false;
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

static int load_firmware_from_flash(struct platform_device *pdev, char **fw_buf)
{
	struct platform_device *flash_leaf = NULL;
	struct flash_data_header header = { 0 };
	const size_t magiclen = sizeof(header.fdh_id_begin.fdi_magic);
	size_t flash_size = 0;
	int ret = 0;
	char *buf = NULL;
	struct flash_data_ident id = { 0 };
	struct xocl_flash_ioctl_read frd = { 0 };

	xocl_info(pdev, "try loading fw from flash");

	flash_leaf = xocl_subdev_get_leaf_by_id(pdev, XOCL_SUBDEV_QSPI,
		PLATFORM_DEVID_NONE);
	if (flash_leaf == NULL) {
		xocl_err(pdev, "failed to hold flash leaf");
		return -ENODEV;
	}

	(void) xocl_subdev_ioctl(flash_leaf, XOCL_FLASH_GET_SIZE, &flash_size);
	if (flash_size == 0) {
		xocl_err(pdev, "failed to get flash size");
		ret = -EINVAL;
		goto done;
	}

	frd.xfir_buf = (char *)&header;
	frd.xfir_size = sizeof(header);
	frd.xfir_offset = flash_size - sizeof(header);
	ret = xocl_subdev_ioctl(flash_leaf, XOCL_FLASH_READ, &frd);
	if (ret) {
		xocl_err(pdev, "failed to read header from flash: %d", ret);
		goto done;
	}

	/* Pick the end ident since header is aligned in the end of flash. */
	id = header.fdh_id_end;
	if (strncmp(id.fdi_magic, XRT_DATA_MAGIC, magiclen)) {
		char tmp[sizeof(id.fdi_magic) + 1] = { 0 };

		memcpy(tmp, id.fdi_magic, magiclen);
		xocl_info(pdev, "ignore meta data, bad magic: %s", tmp);
		ret = -ENOENT;
		goto done;
	}
	if (id.fdi_version != 0) {
		xocl_info(pdev, "flash meta data version is not supported: %d",
			id.fdi_version);
		ret = -EOPNOTSUPP;
		goto done;
	}

	buf = vmalloc(header.fdh_data_len);
	if (buf == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	frd.xfir_buf = buf;
	frd.xfir_size = header.fdh_data_len;
	frd.xfir_offset = header.fdh_data_offset;
	ret = xocl_subdev_ioctl(flash_leaf, XOCL_FLASH_READ, &frd);
	if (ret) {
		xocl_err(pdev, "failed to read meta data from flash: %d", ret);
		goto done;
	} else if (flash_xrt_data_get_parity32(buf, header.fdh_data_len) ^
		header.fdh_data_parity) {
		xocl_err(pdev, "meta data is corrupted");
		ret = -EINVAL;
		goto done;
	}

	xocl_info(pdev, "found meta data of %d bytes @0x%x",
		header.fdh_data_len, header.fdh_data_offset);
	*fw_buf = buf;

done:
	(void) xocl_subdev_put_leaf(pdev, flash_leaf);
	return ret;
}

static int load_firmware_from_disk(struct platform_device *pdev, char **fw_buf)
{
	char uuid[16];
	int err = 0, count = 0, i;
	char fw_name[256];
	const struct firmware *fw;
	struct platform_device *gpio_leaf;
	struct xocl_gpio_ioctl_rw gpio_arg = { 0 };

	gpio_leaf = xocl_subdev_get_leaf(pdev, xocl_gpio_match_epname,
		NODE_BLP_ROM);
	if (!gpio_leaf) {
		xocl_err(pdev, "can not get %s", NODE_BLP_ROM);
		return -EFAULT;
	}

	gpio_arg.xgir_id = XOCL_GPIO_UUID;
	gpio_arg.xgir_buf = uuid;
	gpio_arg.xgir_len = sizeof(uuid);
	gpio_arg.xgir_offset = 0;
	err = xocl_subdev_ioctl(gpio_leaf, XOCL_GPIO_READ, &gpio_arg);
	if (err) {
		xocl_err(pdev, "can not get uuid");
		xocl_subdev_put_leaf(pdev, gpio_leaf);
		return -EFAULT;
	}
	xocl_subdev_put_leaf(pdev, gpio_leaf);
	count = snprintf(fw_name, sizeof(fw_name), "xilinx/");
	for (i = sizeof(uuid) - sizeof(32); i >= 0; i -= sizeof(u32)) {
		count += snprintf(fw_name + count, sizeof(fw_name) - count,
			"%08x", *(u32 *)&uuid[i]);
	}
	count += snprintf(fw_name + count, sizeof(fw_name) - count,
		"/partition.xsabin");

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

static int xmgmt_create_blp(struct xmgmt_main *xmm)
{
	struct platform_device *pdev = xmm->pdev;
	int rc = 0;
	void *dtb = NULL;

	rc = xrt_xclbin_get_section(xmm->firmware, PARTITION_METADATA,
		&dtb, NULL);
	if (rc) {
		xocl_err(pdev, "failed to find BLP dtb");
	} else {
		rc = xocl_subdev_create_partition(pdev, dtb);
		if (rc < 0)
			xocl_err(pdev, "failed to create BLP: %d", rc);
		else
			rc = 0;
		vfree(dtb);
	}
	return rc;
}

static int xmgmt_main_event_cb(struct platform_device *pdev,
	enum xocl_events evt, enum xocl_subdev_id id, int instance)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	int rc;

	xocl_info(pdev, "event %d for (%d, %d)", evt, id, instance);

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		break;
	default:
		return 0;
	}

	if (id == XOCL_SUBDEV_GPIO) {
		xmm->gpio_ready = true;
		rc = load_firmware_from_disk(pdev, &xmm->firmware);
		if (rc == 0) {
			(void) xmgmt_create_blp(xmm);
			xmm->evt_hdl = NULL;
			return 1; /* will not notify any more */
		}
		/*
		 * if firmware is not on disk, need to wait for flash driver
		 * to be online so that we can try to load it from flash.
		 */
		if (!xmm->flash_ready)
			return 0;
		rc = load_firmware_from_flash(pdev, &xmm->firmware);
		if (rc == 0) {
			(void) xmgmt_create_blp(xmm);
			xmm->evt_hdl = NULL;
			return 1; /* will not notify any more */
		}

		return 0;
	}

	xmm->flash_ready = true;
	if (!xmm->gpio_ready)
		return 0;

	rc = load_firmware_from_flash(pdev, &xmm->firmware);
	if (rc == 0)
		(void) xmgmt_create_blp(xmm);

	xmm->evt_hdl = NULL;
	return 1;
}

static int xmgmt_main_probe(struct platform_device *pdev)
{
	struct xmgmt_main *xmm;

	xocl_info(pdev, "probing...");

	xmm = devm_kzalloc(DEV(pdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->pdev = pdev;
	platform_set_drvdata(pdev, xmm);
	mutex_init(&xmm->busy_mutex);

	xmm->evt_hdl = xocl_subdev_add_event_cb(pdev,
		xmgmt_main_leaf_match, NODE_BLP_ROM, xmgmt_main_event_cb);

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

static long xmgmt_main_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
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
