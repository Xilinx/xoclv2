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
#include <linux/uaccess.h>
#include "xocl-xclbin.h"
#include "xocl-metadata.h"
#include "xocl-flash.h"
#include "xocl-subdev.h"
#include "uapi/flash_xrt_data.h"
#include "uapi/xmgmt-ioctl.h"
#include "xocl-gpio.h"
#include "xmgmt-main.h"
#include "xmgmt-fmgr.h"
#include "xocl-icap.h"
#include "xocl-axigate.h"
#include "xmgmt-main-impl.h"

#define	XMGMT_MAIN "xmgmt_main"

struct xmgmt_main {
	struct platform_device *pdev;
	void *evt_hdl;
	char *firmware_blp;
	char *firmware_plp;
	char *firmware_ulp;
	bool flash_ready;
	bool gpio_ready;
	struct fpga_manager *fmgr;
	struct mutex busy_mutex;

	uuid_t *blp_intf_uuids;
	u32 blp_intf_uuid_num;
};

static char *xmgmt_get_vbnv(struct xmgmt_main *xmm)
{
	char *vbnv;
	char *ret;
	int i;

	if (xmm->firmware_plp) {
		vbnv = ((struct axlf *)xmm->firmware_plp)->
			m_header.m_platformVBNV;
	} else if (xmm->firmware_blp) {
		vbnv = ((struct axlf *)xmm->firmware_blp)->
			m_header.m_platformVBNV;
	} else {
		return NULL;
	}

	ret = kstrdup(vbnv, GFP_KERNEL);
	for (i = 0; i < strlen(vbnv); i++) {
		if (vbnv[i] == ':' || vbnv[i] == '.')
			vbnv[i] = '_';
	}
	return ret;
}

static bool xmgmt_main_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	if (id == XOCL_SUBDEV_GPIO)
		return xocl_subdev_match_epname(id, pdev, arg);
	else if (id == XOCL_SUBDEV_QSPI)
		return true;

	return false;
}

static int get_dev_uuid(struct platform_device *pdev, char *uuidstr, size_t len)
{
	char uuid[16];
	struct platform_device *gpio_leaf;
	struct xocl_gpio_ioctl_rw gpio_arg = { 0 };
	int err, i, count;

	gpio_leaf = xocl_subdev_get_leaf(pdev, xocl_subdev_match_epname,
		NODE_BLP_ROM);
	if (!gpio_leaf) {
		xocl_err(pdev, "can not get %s", NODE_BLP_ROM);
		return -EINVAL;
	}

	gpio_arg.xgir_id = XOCL_GPIO_ROM_UUID;
	gpio_arg.xgir_buf = uuid;
	gpio_arg.xgir_len = sizeof(uuid);
	gpio_arg.xgir_offset = 0;
	err = xocl_subdev_ioctl(gpio_leaf, XOCL_GPIO_READ, &gpio_arg);
	xocl_subdev_put_leaf(pdev, gpio_leaf);
	if (err) {
		xocl_err(pdev, "can not get uuid: %d", err);
		return err;
	}

	for (count = 0, i = sizeof(uuid) - sizeof(u32);
		i >= 0 && len > count; i -= sizeof(u32)) {
		count += snprintf(uuidstr + count, len - count,
			"%08x", *(u32 *)&uuid[i]);
	}
	return 0;
}

static ssize_t reset_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);

	if (xocl_subdev_broadcast_event(pdev, XOCL_EVENT_PRE_HOT_RESET) == 0) {
		xocl_subdev_broadcast_event(pdev, XOCL_EVENT_PRE_HOT_RESET);
		(void) xocl_subdev_hot_reset(pdev);
	} else {
		xocl_err(pdev, "offline failed, hot reset is canceled");
	}
	xocl_subdev_broadcast_event(pdev, XOCL_EVENT_POST_HOT_RESET);
	return count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t VBNV_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	ssize_t ret;
	char *vbnv;
	struct platform_device *pdev = to_platform_device(dev);
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);

	vbnv = xmgmt_get_vbnv(xmm);
	ret = sprintf(buf, "%s\n", vbnv);
	kfree(vbnv);
	return ret;
}
static DEVICE_ATTR_RO(VBNV);

static ssize_t logic_uuids_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	ssize_t ret;
	char uuid[80];
	struct platform_device *pdev = to_platform_device(dev);

	/*
	 * Getting UUID pointed to by VSEC,
	 * should be the same as logic UUID of BLP.
	 * TODO: add PLP logic UUID
	 */
	ret = get_dev_uuid(pdev, uuid, sizeof(uuid));
	if (ret)
		return ret;
	ret = sprintf(buf, "%s\n", uuid);
	return ret;
}
static DEVICE_ATTR_RO(logic_uuids);

static inline void uuid2str(const uuid_t *uuid, char *uuidstr, size_t len)
{
	int i, p;
	u8 *u = (u8 *)uuid;

	BUG_ON(sizeof(uuid_t) * 2 + 1 > len);
	for (p = 0, i = sizeof(uuid_t) - 1; i >= 0; p++, i--)
		(void) snprintf(&uuidstr[p*2], 3, "%02x", u[i]);
}

static ssize_t interface_uuids_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	ssize_t ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	u32 i;

	/*
	 * TODO: add PLP interface UUID
	 */
	for (i = 0; i < xmm->blp_intf_uuid_num; i++) {
		char uuidstr[80];

		uuid2str(&xmm->blp_intf_uuids[i], uuidstr, sizeof(uuidstr));
		ret += sprintf(buf + ret, "%s\n", uuidstr);
	}
	return ret;
}
static DEVICE_ATTR_RO(interface_uuids);

static struct attribute *xmgmt_main_attrs[] = {
	&dev_attr_reset.attr,
	&dev_attr_VBNV.attr,
	&dev_attr_logic_uuids.attr,
	&dev_attr_interface_uuids.attr,
	NULL,
};

static ssize_t ulp_image_write(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buffer, loff_t off, size_t count)
{
	struct xmgmt_main *xmm =
		dev_get_drvdata(container_of(kobj, struct device, kobj));
	struct axlf *xclbin;
	ulong len;

	if (off == 0) {
		if (count < sizeof(*xclbin)) {
			xocl_err(xmm->pdev, "count is too small %ld", count);
			return -EINVAL;
		}

		if (xmm->firmware_ulp) {
			vfree(xmm->firmware_ulp);
			xmm->firmware_ulp = NULL;
		}
		xclbin = (struct axlf *)buffer;
		xmm->firmware_ulp = vmalloc(xclbin->m_header.m_length);
		if (!xmm->firmware_ulp)
			return -ENOMEM;
	} else
		xclbin = (struct axlf *)xmm->firmware_ulp;

	len = xclbin->m_header.m_length;
	if (off + count >= len && off < len) {
		memcpy(xmm->firmware_ulp + off, buffer, len - off);
		xmgmt_impl_ulp_download(xmm->pdev, xmm->firmware_ulp);
	} else if (off + count < len) {
		memcpy(xmm->firmware_ulp + off, buffer, count);
	}

	return count;
}

static struct bin_attribute ulp_image_attr = {
	.attr = {
		.name = "ulp_image",
		.mode = 0200
	},
	.write = ulp_image_write,
	.size = 0
};

static struct bin_attribute *xmgmt_main_bin_attrs[] = {
	&ulp_image_attr,
	NULL,
};

static const struct attribute_group xmgmt_main_attrgroup = {
	.attrs = xmgmt_main_attrs,
	.bin_attrs = xmgmt_main_bin_attrs,
};

static int load_firmware_from_flash(struct platform_device *pdev,
	char **fw_buf, size_t *len)
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
	*len = header.fdh_data_len;

done:
	(void) xocl_subdev_put_leaf(pdev, flash_leaf);
	return ret;
}

static int load_firmware_from_disk(struct platform_device *pdev, char **fw_buf,
	size_t *len)
{
	char uuid[80];
	int err = 0;
	char fw_name[256];
	const struct firmware *fw;

	err = get_dev_uuid(pdev, uuid, sizeof(uuid));
	if (err)
		return err;

	(void) snprintf(fw_name,
		sizeof(fw_name), "xilinx/%s/partition.xsabin", uuid);
	xocl_info(pdev, "try loading fw: %s", fw_name);

	err = request_firmware(&fw, fw_name, DEV(pdev));
	if (err)
		return err;

	*fw_buf = vmalloc(fw->size);
	*len = fw->size;
	if (*fw_buf != NULL)
		memcpy(*fw_buf, fw->data, fw->size);
	else
		err = -ENOMEM;

	release_firmware(fw);
	return 0;
}

static const char *get_uuid_from_firmware(struct platform_device *pdev,
	const char *axlf)
{
	const void *uuid = NULL;
	const void *uuiddup = NULL;
	void *dtb = NULL;
	int rc;

	rc = xrt_xclbin_get_section(axlf, PARTITION_METADATA, &dtb, NULL);
	if (rc)
		return NULL;

	rc = xocl_md_get_prop(DEV(pdev), dtb, NULL, NULL,
		PROP_LOGIC_UUID, &uuid, NULL);
	if (!rc)
		uuiddup = kstrdup(uuid, GFP_KERNEL);
	vfree(dtb);
	return uuiddup;
}

static bool is_valid_firmware(struct platform_device *pdev,
	char *fw_buf, size_t fw_len)
{
	struct axlf *axlf = (struct axlf *)fw_buf;
	size_t axlflen = axlf->m_header.m_length;
	const char *fw_uuid;
	char dev_uuid[80];
	int err;

	err = get_dev_uuid(pdev, dev_uuid, sizeof(dev_uuid));
	if (err)
		return false;

	if (memcmp(fw_buf, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)) != 0) {
		xocl_err(pdev, "unknown fw format");
		return false;
	}

	if (axlflen > fw_len) {
		xocl_err(pdev, "truncated fw, length: %ld, expect: %ld",
			fw_len, axlflen);
		return false;
	}

	fw_uuid = get_uuid_from_firmware(pdev, fw_buf);
	if (fw_uuid == NULL || strcmp(fw_uuid, dev_uuid) != 0) {
		xocl_err(pdev, "bad fw UUID: %s, expect: %s",
			fw_uuid ? fw_uuid : "<none>", dev_uuid);
		kfree(fw_uuid);
		return false;
	}

	kfree(fw_uuid);
	return true;
}

static int xmgmt_create_blp(struct xmgmt_main *xmm)
{
	struct platform_device *pdev = xmm->pdev;
	int rc = 0;
	char *dtb = NULL;

	rc = xrt_xclbin_get_metadata(DEV(xmm->pdev), xmm->firmware_blp, &dtb);
	if (rc) {
		xocl_err(pdev, "failed to find BLP dtb: %d", rc);
	} else {
		rc = xocl_subdev_create_partition(pdev, dtb);
		if (rc < 0)
			xocl_err(pdev, "failed to create BLP: %d", rc);
		else
			rc = 0;

		BUG_ON(xmm->blp_intf_uuids);
		xocl_md_get_intf_uuids(&pdev->dev, dtb,
			&xmm->blp_intf_uuid_num, NULL);
		if (xmm->blp_intf_uuid_num > 0) {
			xmm->blp_intf_uuids = vzalloc(sizeof(uuid_t) *
				xmm->blp_intf_uuid_num);
			xocl_md_get_intf_uuids(&pdev->dev, dtb,
				&xmm->blp_intf_uuid_num, xmm->blp_intf_uuids);
		}
		vfree(dtb);
	}
	return rc;
}

static int xmgmt_main_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;
	enum xocl_subdev_id id;
	int instance;
	size_t fwlen;

	switch (evt) {
	case XOCL_EVENT_POST_CREATION: {
		id = esd->xevt_subdev_id;
		instance = esd->xevt_subdev_instance;
		xocl_info(pdev, "processing event %d for (%d, %d)",
			evt, id, instance);

		if (id == XOCL_SUBDEV_GPIO)
			xmm->gpio_ready = true;
		else if (id == XOCL_SUBDEV_QSPI)
			xmm->flash_ready = true;
		else
			BUG_ON(1);

		if (xmm->gpio_ready && xmm->flash_ready) {
			int rc;

			rc = load_firmware_from_disk(pdev, &xmm->firmware_blp,
				&fwlen);
			if (rc != 0) {
				rc = load_firmware_from_flash(pdev,
					&xmm->firmware_blp, &fwlen);
			}
			if (rc == 0 && is_valid_firmware(pdev,
			    xmm->firmware_blp, fwlen))
				(void) xmgmt_create_blp(xmm);
			else
				xocl_err(pdev,
					"failed to find firmware, giving up");
			xmm->evt_hdl = NULL;
			return XOCL_EVENT_CB_STOP;
		}
		break;
	}
	default:
		xocl_info(pdev, "ignored event %d", evt);
		break;
	}

	return XOCL_EVENT_CB_CONTINUE;
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
	xmm->fmgr = xmgmt_fmgr_probe(pdev);
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
	vfree(xmm->blp_intf_uuids);
	vfree(xmm->firmware_blp);
	vfree(xmm->firmware_plp);
	vfree(xmm->firmware_ulp);
	(void) xmgmt_fmgr_remove(xmm->fmgr);
	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xmgmt_main_attrgroup);
	return 0;
}

static int
xmgmt_main_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	int ret = 0;

	xocl_info(pdev, "handling IOCTL cmd: %d", cmd);

	switch (cmd) {
	case XOCL_MGMT_MAIN_GET_XSABIN_SECTION: {
		struct xocl_mgmt_main_ioctl_get_axlf_section *get =
			(struct xocl_mgmt_main_ioctl_get_axlf_section *)arg;

		ret = xrt_xclbin_get_section(xmm->firmware_blp,
			get->xmmigas_section_kind, &get->xmmigas_section,
			&get->xmmigas_section_size);
		break;
	}
	case XOCL_MGMT_MAIN_GET_VBNV: {
		char **vbnv_p = (char **)arg;

		*vbnv_p = xmgmt_get_vbnv(xmm);
		break;
	}
	case XOCL_MGMT_MAIN_GET_ULP_SECTION: {
		struct xocl_mgmt_main_ioctl_get_axlf_section *get =
			(struct xocl_mgmt_main_ioctl_get_axlf_section *)arg;

		ret = xrt_xclbin_get_section(xmm->firmware_ulp,
			get->xmmigas_section_kind, &get->xmmigas_section,
			&get->xmmigas_section_size);
		break;
	}
	default:
		xocl_err(pdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
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

static int bitstream_axlf_ioctl(struct xmgmt_main *xmm, const void __user *arg)
{
	struct fpga_image_info info = { 0 };
	void *copy_buffer = NULL;
	size_t copy_buffer_size = 0;
	struct xclmgmt_ioc_bitstream_axlf ioc_obj = { 0 };
	struct axlf xclbin_obj = { {0} };
	int ret = 0;

	if (copy_from_user((void *)&ioc_obj, arg, sizeof(ioc_obj)))
		return -EFAULT;
	if (copy_from_user((void *)&xclbin_obj, ioc_obj.xclbin,
		sizeof(xclbin_obj)))
		return -EFAULT;
	if (memcmp(xclbin_obj.m_magic, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)))
		return -EINVAL;

	copy_buffer_size = xclbin_obj.m_header.m_length;
	/* Assuming xclbin is not over 1G */
	if (copy_buffer_size > 1024 * 1024 * 1024)
		return -EINVAL;
	copy_buffer = vmalloc(copy_buffer_size);
	if (copy_buffer == NULL)
		return -ENOMEM;

	if (copy_from_user(copy_buffer, ioc_obj.xclbin, copy_buffer_size)) {
		vfree(copy_buffer);
		return -EFAULT;
	}

	info.buf = (char *)copy_buffer;
	info.count = copy_buffer_size;
	ret = fpga_mgr_load(xmm->fmgr, &info);
	if (ret) {
		vfree(copy_buffer);
		return ret;
	}

	vfree(xmm->firmware_ulp);
	xmm->firmware_ulp = copy_buffer;
	return ret;
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
		result = bitstream_axlf_ioctl(xmm, (const void __user *)arg);
		break;
	default:
		result = -ENOTTY;
		break;
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
