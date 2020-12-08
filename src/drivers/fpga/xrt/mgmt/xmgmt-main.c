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
#include "xrt-xclbin.h"
#include "xrt-metadata.h"
#include "xrt-flash.h"
#include "xrt-subdev.h"
#include <linux/xrt/flash_xrt_data.h>
#include <linux/xrt/xmgmt-ioctl.h>
#include "xrt-gpio.h"
#include "xmgmt-main.h"
#include "xmgmt-fmgr.h"
#include "xrt-icap.h"
#include "xrt-axigate.h"
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
	void *mailbox_hdl;
	struct mutex busy_mutex;

	uuid_t *blp_intf_uuids;
	u32 blp_intf_uuid_num;
};

char *xmgmt_get_vbnv(struct platform_device *pdev)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	const char *vbnv;
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
	for (i = 0; i < strlen(ret); i++) {
		if (ret[i] == ':' || ret[i] == '.')
			ret[i] = '_';
	}
	return ret;
}

static bool xmgmt_main_leaf_match(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	if (id == XRT_SUBDEV_GPIO)
		return xrt_subdev_has_epname(pdev, arg);
	else if (id == XRT_SUBDEV_QSPI)
		return true;

	return false;
}

static int get_dev_uuid(struct platform_device *pdev, char *uuidstr, size_t len)
{
	char uuid[16];
	struct platform_device *gpio_leaf;
	struct xrt_gpio_ioctl_rw gpio_arg = { 0 };
	int err, i, count;

	gpio_leaf = xrt_subdev_get_leaf_by_epname(pdev, NODE_BLP_ROM);
	if (!gpio_leaf) {
		xrt_err(pdev, "can not get %s", NODE_BLP_ROM);
		return -EINVAL;
	}

	gpio_arg.xgir_id = XRT_GPIO_ROM_UUID;
	gpio_arg.xgir_buf = uuid;
	gpio_arg.xgir_len = sizeof(uuid);
	gpio_arg.xgir_offset = 0;
	err = xrt_subdev_ioctl(gpio_leaf, XRT_GPIO_READ, &gpio_arg);
	xrt_subdev_put_leaf(pdev, gpio_leaf);
	if (err) {
		xrt_err(pdev, "can not get uuid: %d", err);
		return err;
	}

	for (count = 0, i = sizeof(uuid) - sizeof(u32);
		i >= 0 && len > count; i -= sizeof(u32)) {
		count += snprintf(uuidstr + count, len - count,
			"%08x", *(u32 *)&uuid[i]);
	}
	return 0;
}

int xmgmt_hot_reset(struct platform_device *pdev)
{
	int ret = xrt_subdev_broadcast_event(pdev, XRT_EVENT_PRE_HOT_RESET);

	if (ret) {
		xrt_err(pdev, "offline failed, hot reset is canceled");
		return ret;
	}

	(void) xrt_subdev_hot_reset(pdev);
	xrt_subdev_broadcast_event(pdev, XRT_EVENT_POST_HOT_RESET);
	return 0;
}

static ssize_t reset_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);

	(void) xmgmt_hot_reset(pdev);
	return count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t VBNV_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	ssize_t ret;
	char *vbnv;
	struct platform_device *pdev = to_platform_device(dev);

	vbnv = xmgmt_get_vbnv(pdev);
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
			xrt_err(xmm->pdev, "count is too small %ld", count);
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
		xmgmt_ulp_download(xmm->pdev, xmm->firmware_ulp);
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
	struct xrt_flash_ioctl_read frd = { 0 };

	xrt_info(pdev, "try loading fw from flash");

	flash_leaf = xrt_subdev_get_leaf_by_id(pdev, XRT_SUBDEV_QSPI,
		PLATFORM_DEVID_NONE);
	if (flash_leaf == NULL) {
		xrt_err(pdev, "failed to hold flash leaf");
		return -ENODEV;
	}

	(void) xrt_subdev_ioctl(flash_leaf, XRT_FLASH_GET_SIZE, &flash_size);
	if (flash_size == 0) {
		xrt_err(pdev, "failed to get flash size");
		ret = -EINVAL;
		goto done;
	}

	frd.xfir_buf = (char *)&header;
	frd.xfir_size = sizeof(header);
	frd.xfir_offset = flash_size - sizeof(header);
	ret = xrt_subdev_ioctl(flash_leaf, XRT_FLASH_READ, &frd);
	if (ret) {
		xrt_err(pdev, "failed to read header from flash: %d", ret);
		goto done;
	}

	/* Pick the end ident since header is aligned in the end of flash. */
	id = header.fdh_id_end;
	if (strncmp(id.fdi_magic, XRT_DATA_MAGIC, magiclen)) {
		char tmp[sizeof(id.fdi_magic) + 1] = { 0 };

		memcpy(tmp, id.fdi_magic, magiclen);
		xrt_info(pdev, "ignore meta data, bad magic: %s", tmp);
		ret = -ENOENT;
		goto done;
	}
	if (id.fdi_version != 0) {
		xrt_info(pdev, "flash meta data version is not supported: %d",
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
	ret = xrt_subdev_ioctl(flash_leaf, XRT_FLASH_READ, &frd);
	if (ret) {
		xrt_err(pdev, "failed to read meta data from flash: %d", ret);
		goto done;
	} else if (flash_xrt_data_get_parity32(buf, header.fdh_data_len) ^
		header.fdh_data_parity) {
		xrt_err(pdev, "meta data is corrupted");
		ret = -EINVAL;
		goto done;
	}

	xrt_info(pdev, "found meta data of %d bytes @0x%x",
		header.fdh_data_len, header.fdh_data_offset);
	*fw_buf = buf;
	*len = header.fdh_data_len;

done:
	(void) xrt_subdev_put_leaf(pdev, flash_leaf);
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
	xrt_info(pdev, "try loading fw: %s", fw_name);

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

static const char *xmgmt_get_axlf_firmware(struct xmgmt_main *xmm,
	enum provider_kind kind)
{
	switch (kind) {
	case XMGMT_BLP:
		return xmm->firmware_blp;
	case XMGMT_PLP:
		return xmm->firmware_plp;
	case XMGMT_ULP:
		return xmm->firmware_ulp;
	default:
		xrt_err(xmm->pdev, "unknown axlf kind: %d", kind);
		return NULL;
	}
}

char *xmgmt_get_dtb(struct platform_device *pdev, enum provider_kind kind)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	char *dtb = NULL;
	const char *provider = xmgmt_get_axlf_firmware(xmm, kind);
	int rc;

	if (provider == NULL)
		return dtb;

	rc = xrt_xclbin_get_metadata(DEV(pdev), provider, &dtb);
	if (rc)
		xrt_err(pdev, "failed to find dtb: %d", rc);
	return dtb;
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

	rc = xrt_md_get_prop(DEV(pdev), dtb, NULL, NULL,
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
		xrt_err(pdev, "unknown fw format");
		return false;
	}

	if (axlflen > fw_len) {
		xrt_err(pdev, "truncated fw, length: %ld, expect: %ld",
			fw_len, axlflen);
		return false;
	}

	fw_uuid = get_uuid_from_firmware(pdev, fw_buf);
	if (fw_uuid == NULL || strcmp(fw_uuid, dev_uuid) != 0) {
		xrt_err(pdev, "bad fw UUID: %s, expect: %s",
			fw_uuid ? fw_uuid : "<none>", dev_uuid);
		kfree(fw_uuid);
		return false;
	}

	kfree(fw_uuid);
	return true;
}

int xmgmt_get_provider_uuid(struct platform_device *pdev,
	enum provider_kind kind, uuid_t *uuid)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	const char *fwbuf;
	const char *fw_uuid;
	int rc = -ENOENT;

	mutex_lock(&xmm->busy_mutex);

	fwbuf = xmgmt_get_axlf_firmware(xmm, kind);
	if (fwbuf == NULL)
		goto done;

	fw_uuid = get_uuid_from_firmware(pdev, fwbuf);
	if (fw_uuid == NULL)
		goto done;

	rc = xrt_md_uuid_strtoid(DEV(pdev), fw_uuid, uuid);
	kfree(fw_uuid);

done:
	mutex_unlock(&xmm->busy_mutex);
	return rc;
}

static int xmgmt_create_blp(struct xmgmt_main *xmm)
{
	struct platform_device *pdev = xmm->pdev;
	int rc = 0;
	char *dtb = NULL;

	dtb = xmgmt_get_dtb(pdev, XMGMT_BLP);
	if (dtb) {
		rc = xrt_subdev_create_partition(pdev, dtb);
		if (rc < 0)
			xrt_err(pdev, "failed to create BLP: %d", rc);
		else
			rc = 0;

		BUG_ON(xmm->blp_intf_uuids);
		xrt_md_get_intf_uuids(&pdev->dev, dtb,
			&xmm->blp_intf_uuid_num, NULL);
		if (xmm->blp_intf_uuid_num > 0) {
			xmm->blp_intf_uuids = vzalloc(sizeof(uuid_t) *
				xmm->blp_intf_uuid_num);
			xrt_md_get_intf_uuids(&pdev->dev, dtb,
				&xmm->blp_intf_uuid_num, xmm->blp_intf_uuids);
		}
	}

	vfree(dtb);
	return rc;
}

static int xmgmt_main_event_cb(struct platform_device *pdev,
	enum xrt_events evt, void *arg)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	struct xrt_event_arg_subdev *esd = (struct xrt_event_arg_subdev *)arg;
	enum xrt_subdev_id id;
	int instance;
	size_t fwlen;

	switch (evt) {
	case XRT_EVENT_POST_CREATION: {
		id = esd->xevt_subdev_id;
		instance = esd->xevt_subdev_instance;
		xrt_info(pdev, "processing event %d for (%d, %d)",
			evt, id, instance);

		if (id == XRT_SUBDEV_GPIO)
			xmm->gpio_ready = true;
		else if (id == XRT_SUBDEV_QSPI)
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
				xrt_err(pdev,
					"failed to find firmware, giving up");
			xmm->evt_hdl = NULL;
		}
		break;
	}
	case XRT_EVENT_POST_ATTACH:
		xmgmt_peer_notify_state(xmm->mailbox_hdl, true);
		break;
	case XRT_EVENT_PRE_DETACH:
		xmgmt_peer_notify_state(xmm->mailbox_hdl, false);
		break;
	default:
		xrt_info(pdev, "ignored event %d", evt);
		break;
	}

	return XRT_EVENT_CB_CONTINUE;
}

static int xmgmt_main_probe(struct platform_device *pdev)
{
	struct xmgmt_main *xmm;

	xrt_info(pdev, "probing...");

	xmm = devm_kzalloc(DEV(pdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->pdev = pdev;
	platform_set_drvdata(pdev, xmm);
	xmm->fmgr = xmgmt_fmgr_probe(pdev);
	xmm->mailbox_hdl = xmgmt_mailbox_probe(pdev);
	mutex_init(&xmm->busy_mutex);

	xmm->evt_hdl = xrt_subdev_add_event_cb(pdev,
		xmgmt_main_leaf_match, NODE_BLP_ROM, xmgmt_main_event_cb);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(pdev)->kobj, &xmgmt_main_attrgroup))
		xrt_err(pdev, "failed to create sysfs group");
	return 0;
}

static int xmgmt_main_remove(struct platform_device *pdev)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);

	/* By now, partition driver should prevent any inter-leaf call. */

	xrt_info(pdev, "leaving...");

	if (xmm->evt_hdl)
		(void) xrt_subdev_remove_event_cb(pdev, xmm->evt_hdl);
	vfree(xmm->blp_intf_uuids);
	vfree(xmm->firmware_blp);
	vfree(xmm->firmware_plp);
	vfree(xmm->firmware_ulp);
	(void) xmgmt_fmgr_remove(xmm->fmgr);
	xmgmt_mailbox_remove(xmm->mailbox_hdl);
	(void) sysfs_remove_group(&DEV(pdev)->kobj, &xmgmt_main_attrgroup);
	return 0;
}

static int
xmgmt_main_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	int ret = 0;

	xrt_info(pdev, "handling IOCTL cmd: %d", cmd);

	switch (cmd) {
	case XRT_MGMT_MAIN_GET_AXLF_SECTION: {
		struct xrt_mgmt_main_ioctl_get_axlf_section *get =
			(struct xrt_mgmt_main_ioctl_get_axlf_section *)arg;
		const char *firmware =
			xmgmt_get_axlf_firmware(xmm, get->xmmigas_axlf_kind);

		if (firmware == NULL) {
			ret = -ENOENT;
		} else {
			ret = xrt_xclbin_get_section(firmware,
				get->xmmigas_section_kind,
				&get->xmmigas_section,
				&get->xmmigas_section_size);
		}
		break;
	}
	case XRT_MGMT_MAIN_GET_VBNV: {
		char **vbnv_p = (char **)arg;

		*vbnv_p = xmgmt_get_vbnv(pdev);
		break;
	}
	default:
		xrt_err(pdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int xmgmt_main_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xrt_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xrt_info(pdev, "opened");
	file->private_data = platform_get_drvdata(pdev);
	return 0;
}

static int xmgmt_main_close(struct inode *inode, struct file *file)
{
	struct xmgmt_main *xmm = file->private_data;

	xrt_devnode_close(inode);

	xrt_info(xmm->pdev, "closed");
	return 0;
}

static int xmgmt_bitstream_axlf_fpga_mgr(struct xmgmt_main *xmm,
	void *axlf, size_t size)
{
	int ret;
	struct fpga_image_info info = { 0 };

	BUG_ON(!mutex_is_locked(&xmm->busy_mutex));

	/*
	 * Should any error happens during download, we can't trust
	 * the cached xclbin any more.
	 */
	vfree(xmm->firmware_ulp);
	xmm->firmware_ulp = NULL;

	info.buf = (char *)axlf;
	info.count = size;
	ret = fpga_mgr_load(xmm->fmgr, &info);
	if (ret == 0)
		xmm->firmware_ulp = axlf;

	return ret;
}

int bitstream_axlf_mailbox(struct platform_device *pdev, const void *axlf)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);
	void *copy_buffer = NULL;
	size_t copy_buffer_size = 0;
	const struct axlf *xclbin_obj = axlf;
	int ret = 0;

	if (memcmp(xclbin_obj->m_magic, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)))
		return -EINVAL;

	copy_buffer_size = xclbin_obj->m_header.m_length;
	if (copy_buffer_size > MAX_XCLBIN_SIZE)
		return -EINVAL;
	copy_buffer = vmalloc(copy_buffer_size);
	if (copy_buffer == NULL)
		return -ENOMEM;
	(void) memcpy(copy_buffer, axlf, copy_buffer_size);

	mutex_lock(&xmm->busy_mutex);
	ret = xmgmt_bitstream_axlf_fpga_mgr(xmm, copy_buffer, copy_buffer_size);
	mutex_unlock(&xmm->busy_mutex);
	if (ret)
		vfree(copy_buffer);
	return ret;
}

static int bitstream_axlf_ioctl(struct xmgmt_main *xmm, const void __user *arg)
{
	void *copy_buffer = NULL;
	size_t copy_buffer_size = 0;
	struct xmgmt_ioc_bitstream_axlf ioc_obj = { 0 };
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
	if (copy_buffer_size > MAX_XCLBIN_SIZE)
		return -EINVAL;
	copy_buffer = vmalloc(copy_buffer_size);
	if (copy_buffer == NULL)
		return -ENOMEM;

	if (copy_from_user(copy_buffer, ioc_obj.xclbin, copy_buffer_size)) {
		vfree(copy_buffer);
		return -EFAULT;
	}

	ret = xmgmt_bitstream_axlf_fpga_mgr(xmm, copy_buffer, copy_buffer_size);
	if (ret)
		vfree(copy_buffer);

	return ret;
}

static long xmgmt_main_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	long result = 0;
	struct xmgmt_main *xmm = filp->private_data;

	BUG_ON(!xmm);

	if (_IOC_TYPE(cmd) != XMGMT_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&xmm->busy_mutex);

	xrt_info(xmm->pdev, "ioctl cmd %d, arg %ld", cmd, arg);
	switch (cmd) {
	case XMGMT_IOCICAPDOWNLOAD_AXLF:
		result = bitstream_axlf_ioctl(xmm, (const void __user *)arg);
		break;
	default:
		result = -ENOTTY;
		break;
	}

	mutex_unlock(&xmm->busy_mutex);
	return result;
}

void *xmgmt_pdev2mailbox(struct platform_device *pdev)
{
	struct xmgmt_main *xmm = platform_get_drvdata(pdev);

	return xmm->mailbox_hdl;
}

struct xrt_subdev_endpoints xrt_mgmt_main_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = NODE_MGMT_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xrt_subdev_drvdata xmgmt_main_data = {
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
