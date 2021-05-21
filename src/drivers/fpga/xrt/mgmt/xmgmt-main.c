// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA MGMT PF entry point driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *	Sonal Santan <sonals@xilinx.com>
 */

#include <linux/firmware.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "xclbin-helper.h"
#include "metadata.h"
#include "xleaf/flash.h"
#include "xleaf.h"
#include <linux/xrt/flash_xrt_data.h>
#include <linux/xrt/xmgmt-ioctl.h>
#include "xleaf/devctl.h"
#include "xmgmt-main.h"
#include "xrt-mgr.h"
#include "xleaf/icap.h"
#include "xleaf/axigate.h"
#include "xleaf/pcie-firewall.h"
#include "xmgmt.h"

#define XMGMT_MAIN "xmgmt_main"
#define XMGMT_SUPP_XCLBIN_MAJOR 2

#define XMGMT_FLAG_FLASH_READY	1
#define XMGMT_FLAG_DEVCTL_READY	2

#define XMGMT_UUID_STR_LEN	(UUID_SIZE * 2 + 1)

struct xmgmt_main {
	struct xrt_device *xdev;
	struct axlf *firmware_blp;
	struct axlf *firmware_plp;
	struct axlf *firmware_ulp;
	u32 flags;
	struct fpga_manager *fmgr;
	void *mailbox_hdl;
	struct mutex lock; /* busy lock */
	uuid_t *blp_interface_uuids;
	u32 blp_interface_uuid_num;
};

/*
 * VBNV stands for Vendor, BoardID, Name, Version. It is a string
 * which describes board and shell.
 *
 * Caller is responsible for freeing the returned string.
 */
char *xmgmt_get_vbnv(struct xrt_device *xdev)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	const char *vbnv;
	char *ret;
	int i;

	if (xmm->firmware_plp)
		vbnv = xmm->firmware_plp->header.platform_vbnv;
	else if (xmm->firmware_blp)
		vbnv = xmm->firmware_blp->header.platform_vbnv;
	else
		return NULL;

	ret = kstrdup(vbnv, GFP_KERNEL);
	if (!ret)
		return NULL;

	for (i = 0; i < strlen(ret); i++) {
		if (ret[i] == ':' || ret[i] == '.')
			ret[i] = '_';
	}
	return ret;
}

static int get_dev_uuid(struct xrt_device *xdev, char *uuidstr, size_t len)
{
	struct xrt_devctl_rw devctl_arg = { 0 };
	struct xrt_device *devctl_leaf;
	char uuid_buf[UUID_SIZE];
	uuid_t uuid;
	int err;

	devctl_leaf = xleaf_get_leaf_by_epname(xdev, XRT_MD_NODE_BLP_ROM);
	if (!devctl_leaf) {
		xrt_err(xdev, "can not get %s", XRT_MD_NODE_BLP_ROM);
		return -EINVAL;
	}

	devctl_arg.xdr_id = XRT_DEVCTL_ROM_UUID;
	devctl_arg.xdr_buf = uuid_buf;
	devctl_arg.xdr_len = sizeof(uuid_buf);
	devctl_arg.xdr_offset = 0;
	err = xleaf_call(devctl_leaf, XRT_DEVCTL_READ, &devctl_arg);
	xleaf_put_leaf(xdev, devctl_leaf);
	if (err) {
		xrt_err(xdev, "can not get uuid: %d", err);
		return err;
	}
	import_uuid(&uuid, uuid_buf);
	xrt_md_trans_uuid2str(&uuid, uuidstr);

	return 0;
}

int xmgmt_hot_reset(struct xrt_device *xdev)
{
	int ret = xleaf_broadcast_event(xdev, XRT_EVENT_PRE_HOT_RESET, false);

	if (ret) {
		xrt_err(xdev, "offline failed, hot reset is canceled");
		return ret;
	}

	xleaf_hot_reset(xdev);
	xleaf_broadcast_event(xdev, XRT_EVENT_POST_HOT_RESET, false);
	return 0;
}

static ssize_t reset_store(struct device *dev, struct device_attribute *da,
			   const char *buf, size_t count)
{
	struct xrt_device *xdev = to_xrt_dev(dev);

	xmgmt_hot_reset(xdev);
	return count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t VBNV_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	ssize_t ret;
	char *vbnv;

	vbnv = xmgmt_get_vbnv(xdev);
	if (!vbnv)
		return -EINVAL;
	ret = sprintf(buf, "%s\n", vbnv);
	kfree(vbnv);
	return ret;
}
static DEVICE_ATTR_RO(VBNV);

/* logic uuid is the uuid uniquely identfy the partition */
static ssize_t logic_uuids_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	char uuid[XMGMT_UUID_STR_LEN];
	ssize_t ret;

	/* Getting UUID pointed to by VSEC, should be the same as logic UUID of BLP. */
	ret = get_dev_uuid(xdev, uuid, sizeof(uuid));
	if (ret)
		return ret;
	ret = sprintf(buf, "%s\n", uuid);
	return ret;
}
static DEVICE_ATTR_RO(logic_uuids);

static ssize_t interface_uuids_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	ssize_t ret = 0;
	u32 i;

	for (i = 0; i < xmm->blp_interface_uuid_num; i++) {
		char uuidstr[XMGMT_UUID_STR_LEN];

		xrt_md_trans_uuid2str(&xmm->blp_interface_uuids[i], uuidstr);
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

static const struct attribute_group xmgmt_main_attrgroup = {
	.attrs = xmgmt_main_attrs,
};

static int load_firmware_from_flash(struct xrt_device *xdev, struct axlf **fw_buf, size_t *len)
{
	struct xrt_device *flash_leaf = NULL;
	struct flash_data_header header = { 0 };
	const size_t magiclen = sizeof(header.fdh_id_begin.fdi_magic);
	size_t flash_size = 0;
	int ret = 0;
	char *buf = NULL;
	struct flash_data_ident id = { 0 };
	struct xrt_flash_read frd = { 0 };

	xrt_info(xdev, "try loading fw from flash");

	flash_leaf = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_QSPI, PLATFORM_DEVID_NONE);
	if (!flash_leaf) {
		xrt_err(xdev, "failed to hold flash leaf");
		return -ENODEV;
	}

	xleaf_call(flash_leaf, XRT_FLASH_GET_SIZE, &flash_size);
	if (flash_size == 0) {
		xrt_err(xdev, "failed to get flash size");
		ret = -EINVAL;
		goto done;
	}

	frd.xfir_buf = (char *)&header;
	frd.xfir_size = sizeof(header);
	frd.xfir_offset = flash_size - sizeof(header);
	ret = xleaf_call(flash_leaf, XRT_FLASH_READ, &frd);
	if (ret) {
		xrt_err(xdev, "failed to read header from flash: %d", ret);
		goto done;
	}

	/* Pick the end ident since header is aligned in the end of flash. */
	id = header.fdh_id_end;
	if (strncmp(id.fdi_magic, XRT_DATA_MAGIC, magiclen)) {
		char tmp[sizeof(id.fdi_magic) + 1] = { 0 };

		memcpy(tmp, id.fdi_magic, magiclen);
		xrt_info(xdev, "ignore meta data, bad magic: %s", tmp);
		ret = -ENOENT;
		goto done;
	}
	if (id.fdi_version != 0) {
		xrt_info(xdev, "flash meta data version is not supported: %d", id.fdi_version);
		ret = -EOPNOTSUPP;
		goto done;
	}

	buf = vmalloc(header.fdh_data_len);
	if (!buf) {
		ret = -ENOMEM;
		goto done;
	}

	frd.xfir_buf = buf;
	frd.xfir_size = header.fdh_data_len;
	frd.xfir_offset = header.fdh_data_offset;
	ret = xleaf_call(flash_leaf, XRT_FLASH_READ, &frd);
	if (ret) {
		xrt_err(xdev, "failed to read meta data from flash: %d", ret);
		goto done;
	} else if (flash_xrt_data_get_parity32(buf, header.fdh_data_len) ^ header.fdh_data_parity) {
		xrt_err(xdev, "meta data is corrupted");
		ret = -EINVAL;
		goto done;
	}

	xrt_info(xdev, "found meta data of %d bytes @0x%x",
		 header.fdh_data_len, header.fdh_data_offset);
	*fw_buf = (struct axlf *)buf;
	*len = header.fdh_data_len;

done:
	xleaf_put_leaf(xdev, flash_leaf);
	return ret;
}

static int load_firmware_from_disk(struct xrt_device *xdev, struct axlf **fw_buf, size_t *len)
{
	char uuid[XMGMT_UUID_STR_LEN];
	const struct firmware *fw;
	char fw_name[256];
	int err = 0;

	*len = 0;
	err = get_dev_uuid(xdev, uuid, sizeof(uuid));
	if (err)
		return err;

	snprintf(fw_name, sizeof(fw_name), "xilinx/%s/partition.xsabin", uuid);
	xrt_info(xdev, "try loading fw: %s", fw_name);

	err = request_firmware(&fw, fw_name, DEV(xdev));
	if (err)
		return err;

	*fw_buf = vmalloc(fw->size);
	if (!*fw_buf) {
		release_firmware(fw);
		return -ENOMEM;
	}

	*len = fw->size;
	memcpy(*fw_buf, fw->data, fw->size);

	release_firmware(fw);
	return 0;
}

static const struct axlf *xmgmt_get_axlf_firmware(struct xmgmt_main *xmm, enum provider_kind kind)
{
	switch (kind) {
	case XMGMT_BLP:
		return xmm->firmware_blp;
	case XMGMT_PLP:
		return xmm->firmware_plp;
	case XMGMT_ULP:
		return xmm->firmware_ulp;
	default:
		xrt_err(xmm->xdev, "unknown axlf kind: %d", kind);
		return NULL;
	}
}

/* The caller needs to free the returned dtb buffer */
char *xmgmt_get_dtb(struct xrt_device *xdev, enum provider_kind kind)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	const struct axlf *provider;
	char *dtb = NULL;
	int rc;

	provider = xmgmt_get_axlf_firmware(xmm, kind);
	if (!provider)
		return dtb;

	rc = xrt_xclbin_get_metadata(DEV(xdev), provider, &dtb);
	if (rc)
		xrt_err(xdev, "failed to find dtb: %d", rc);
	return dtb;
}

/* The caller needs to free the returned uuid buffer */
static const char *get_uuid_from_firmware(struct xrt_device *xdev, const struct axlf *xclbin)
{
	const void *uuiddup = NULL;
	const void *uuid = NULL;
	void *dtb = NULL;
	int rc;

	rc = xrt_xclbin_get_section(DEV(xdev), xclbin, PARTITION_METADATA, &dtb, NULL);
	if (rc)
		return NULL;

	rc = xrt_md_get_prop(DEV(xdev), dtb, NULL, NULL, XRT_MD_PROP_LOGIC_UUID, &uuid, NULL);
	if (!rc)
		uuiddup = kstrdup(uuid, GFP_KERNEL);
	vfree(dtb);
	return uuiddup;
}

static bool is_valid_firmware(struct xrt_device *xdev,
			      const struct axlf *xclbin, size_t fw_len)
{
	const char *fw_buf = (const char *)xclbin;
	size_t axlflen = xclbin->header.length;
	char dev_uuid[XMGMT_UUID_STR_LEN];
	const char *fw_uuid;
	int err;

	err = get_dev_uuid(xdev, dev_uuid, sizeof(dev_uuid));
	if (err)
		return false;

	if (memcmp(fw_buf, XCLBIN_VERSION2, sizeof(XCLBIN_VERSION2)) != 0) {
		xrt_err(xdev, "unknown fw format");
		return false;
	}

	if (axlflen > fw_len) {
		xrt_err(xdev, "truncated fw, length: %zu, expect: %zu", fw_len, axlflen);
		return false;
	}

	if (xclbin->header.version_major != XMGMT_SUPP_XCLBIN_MAJOR) {
		xrt_err(xdev, "firmware is not supported");
		return false;
	}

	fw_uuid = get_uuid_from_firmware(xdev, xclbin);
	if (!fw_uuid || strncmp(fw_uuid, dev_uuid, sizeof(dev_uuid)) != 0) {
		xrt_err(xdev, "bad fw UUID: %s, expect: %s",
			fw_uuid ? fw_uuid : "<none>", dev_uuid);
		kfree(fw_uuid);
		return false;
	}

	kfree(fw_uuid);
	return true;
}

int xmgmt_get_provider_uuid(struct xrt_device *xdev, enum provider_kind kind, uuid_t *uuid)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	const struct axlf *fwbuf;
	const char *fw_uuid;
	int rc = -ENOENT;

	mutex_lock(&xmm->lock);

	fwbuf = xmgmt_get_axlf_firmware(xmm, kind);
	if (!fwbuf)
		goto done;

	fw_uuid = get_uuid_from_firmware(xdev, fwbuf);
	if (!fw_uuid)
		goto done;

	rc = xrt_md_trans_str2uuid(DEV(xdev), fw_uuid, uuid);
	kfree(fw_uuid);

done:
	mutex_unlock(&xmm->lock);
	return rc;
}

static int xmgmt_unblock_endpoints(struct xrt_device *xdev, char *dtb)
{
	struct xrt_pcie_firewall_unblock arg = { 0 };
	char *epname = NULL, *regmap = NULL;
	struct xrt_device *pcie_firewall;
	struct device *dev = DEV(xdev);
	__be32 *pf_num, *bar_index;
	int rc = 0;

	pcie_firewall = xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_PCIE_FIREWALL,
					     XRT_INVALID_DEVICE_INST);
	if (!pcie_firewall)
		return -ENODEV;

	for (xrt_md_get_next_endpoint(dev, dtb, NULL, NULL, &epname, &regmap);
	     epname;
	     xrt_md_get_next_endpoint(dev, dtb, epname, regmap, &epname, &regmap)) {
		rc = xrt_md_get_prop(dev, dtb, epname, regmap, XRT_MD_PROP_PF_NUM,
				     (const void **)&pf_num, NULL);
		if (rc)
			continue;
		rc = xrt_md_get_prop(dev, dtb, epname, regmap, XRT_MD_PROP_BAR_IDX,
				     (const void **)&bar_index, NULL);
		if (rc)
			bar_index = 0;

		arg.pf_index = be32_to_cpu(*pf_num);
		arg.bar_index = be32_to_cpu(*bar_index);
		rc = xleaf_call(pcie_firewall, XRT_PFW_UNBLOCK, &arg);
		if (rc) {
			/*
			 * It should not fail unless hardware issue. And pci reset
			 * will set pcie firewall to default state. Thus it does not
			 * have to reset pcie firewall on failure case.
			 */
			xrt_err(xdev, "failed to unblock endpoint %s", epname);
			break;
		}
	}
	xleaf_put_leaf(xdev, pcie_firewall);

	return rc;
}

static void xmgmt_unblock_all(struct xrt_device *xdev)
{
	char *dtb;
	int type;

	for (type = XMGMT_BLP; type <= XMGMT_ULP; type++) {
		dtb = xmgmt_get_dtb(xdev, type);
		if (!dtb)
			break;
		xmgmt_unblock_endpoints(xdev, dtb);
	}
}

static int xmgmt_create_blp(struct xmgmt_main *xmm)
{
	const struct axlf *provider = xmgmt_get_axlf_firmware(xmm, XMGMT_BLP);
	struct xrt_device *xdev = xmm->xdev;
	int rc = 0;
	char *dtb = NULL;

	dtb = xmgmt_get_dtb(xdev, XMGMT_BLP);
	if (!dtb) {
		xrt_err(xdev, "did not get BLP metadata");
		return -EINVAL;
	}

	rc = xmgmt_process_xclbin(xmm->xdev, xmm->fmgr, provider, XMGMT_BLP);
	if (rc) {
		xrt_err(xdev, "failed to process BLP: %d", rc);
		goto failed;
	}

	rc = xleaf_create_group(xdev, dtb);
	if (rc < 0) {
		xrt_err(xdev, "failed to create BLP group: %d", rc);
		goto failed;
	}

	WARN_ON(xmm->blp_interface_uuids);
	rc = xrt_md_get_interface_uuids(&xdev->dev, dtb, 0, NULL);
	if (rc > 0) {
		xmm->blp_interface_uuid_num = rc;
		xmm->blp_interface_uuids =
			kcalloc(xmm->blp_interface_uuid_num, sizeof(uuid_t), GFP_KERNEL);
		if (!xmm->blp_interface_uuids) {
			rc = -ENOMEM;
			goto failed;
		}
		xrt_md_get_interface_uuids(&xdev->dev, dtb, xmm->blp_interface_uuid_num,
					   xmm->blp_interface_uuids);
	}

failed:
	vfree(dtb);
	return rc;
}

static int xmgmt_load_firmware(struct xmgmt_main *xmm)
{
	struct xrt_device *xdev = xmm->xdev;
	size_t fwlen;
	int rc;

	rc = load_firmware_from_disk(xdev, &xmm->firmware_blp, &fwlen);
	if (rc != 0)
		rc = load_firmware_from_flash(xdev, &xmm->firmware_blp, &fwlen);
	if (!rc && is_valid_firmware(xdev, xmm->firmware_blp, fwlen))
		xmgmt_create_blp(xmm);
	else
		xrt_err(xdev, "failed to find firmware, giving up: %d", rc);
	return rc;
}

static void xmgmt_main_event_cb(struct xrt_device *xdev, void *arg)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	struct xrt_event *evt = (struct xrt_event *)arg;
	enum xrt_events e = evt->xe_evt;
	struct xrt_device *leaf;
	enum xrt_subdev_id id;

	id = evt->xe_subdev.xevt_subdev_id;
	switch (e) {
	case XRT_EVENT_POST_CREATION: {
		/* mgmt driver finishes attaching, notify user pf. */
		if (id == XRT_ROOT) {
			xmgmt_peer_notify_state(xmm->mailbox_hdl, true);
			break;
		}

		if (id == XRT_SUBDEV_DEVCTL && !(xmm->flags & XMGMT_FLAG_DEVCTL_READY)) {
			leaf = xleaf_get_leaf_by_epname(xdev, XRT_MD_NODE_BLP_ROM);
			if (leaf) {
				xmm->flags |= XMGMT_FLAG_DEVCTL_READY;
				xleaf_put_leaf(xdev, leaf);
			}
		} else if (id == XRT_SUBDEV_QSPI && !(xmm->flags & XMGMT_FLAG_FLASH_READY)) {
			xmm->flags |= XMGMT_FLAG_FLASH_READY;
		} else {
			break;
		}

		if (xmm->flags & XMGMT_FLAG_DEVCTL_READY)
			xmgmt_load_firmware(xmm);
		break;
	}
	case XRT_EVENT_PRE_REMOVAL:
		/* mgmt driver is about to detach, notify user pf. */
		if (id == XRT_ROOT)
			xmgmt_peer_notify_state(xmm->mailbox_hdl, false);
		break;
	case XRT_EVENT_POST_GATE_OPEN:
		xmgmt_unblock_all(xdev);
		break;
	default:
		xrt_dbg(xdev, "ignored event %d", e);
		break;
	}
}

static int xmgmt_main_probe(struct xrt_device *xdev)
{
	struct xmgmt_main *xmm;

	xrt_info(xdev, "probing...");

	xmm = devm_kzalloc(DEV(xdev), sizeof(*xmm), GFP_KERNEL);
	if (!xmm)
		return -ENOMEM;

	xmm->xdev = xdev;
	xmm->fmgr = xmgmt_fmgr_probe(xdev);
	if (IS_ERR(xmm->fmgr))
		return PTR_ERR(xmm->fmgr);

	xrt_set_drvdata(xdev, xmm);
	xmm->mailbox_hdl = xmgmt_mailbox_probe(xdev);
	mutex_init(&xmm->lock);

	/* Ready to handle req thru sysfs nodes. */
	if (sysfs_create_group(&DEV(xdev)->kobj, &xmgmt_main_attrgroup))
		xrt_err(xdev, "failed to create sysfs group");
	return 0;
}

static void xmgmt_main_remove(struct xrt_device *xdev)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);

	/* By now, group driver should prevent any inter-leaf call. */

	xrt_info(xdev, "leaving...");

	kfree(xmm->blp_interface_uuids);
	vfree(xmm->firmware_blp);
	vfree(xmm->firmware_plp);
	vfree(xmm->firmware_ulp);
	xmgmt_region_cleanup_all(xdev);
	xmgmt_fmgr_remove(xmm->fmgr);
	xmgmt_mailbox_remove(xmm->mailbox_hdl);
	sysfs_remove_group(&DEV(xdev)->kobj, &xmgmt_main_attrgroup);
}

static int
xmgmt_mainleaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		xmgmt_mailbox_event_cb(xdev, arg);
		xmgmt_main_event_cb(xdev, arg);
		break;
	case XRT_MGMT_MAIN_GET_AXLF_SECTION: {
		struct xrt_mgmt_main_get_axlf_section *get =
			(struct xrt_mgmt_main_get_axlf_section *)arg;
		const struct axlf *firmware = xmgmt_get_axlf_firmware(xmm, get->xmmigas_axlf_kind);

		if (!firmware) {
			ret = -ENOENT;
		} else {
			ret = xrt_xclbin_get_section(DEV(xdev), firmware,
						     get->xmmigas_section_kind,
						     &get->xmmigas_section,
						     &get->xmmigas_section_size);
		}
		break;
	}
	case XRT_MGMT_MAIN_GET_VBNV: {
		char **vbnv_p = (char **)arg;

		*vbnv_p = xmgmt_get_vbnv(xdev);
		if (!*vbnv_p)
			ret = -EINVAL;
		break;
	}
	default:
		xrt_err(xdev, "unknown cmd: %d", cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int xmgmt_main_open(struct inode *inode, struct file *file)
{
	struct xrt_device *xdev = xleaf_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!xdev)
		return -ENODEV;

	xrt_info(xdev, "opened");
	file->private_data = xrt_get_drvdata(xdev);
	return 0;
}

static int xmgmt_main_close(struct inode *inode, struct file *file)
{
	struct xmgmt_main *xmm = file->private_data;

	xleaf_devnode_close(inode);

	xrt_info(xmm->xdev, "closed");
	return 0;
}

/*
 * Called for xclbin download by either: xclbin load ioctl or
 * peer request from the userpf driver over mailbox.
 */
static int xmgmt_bitstream_axlf_fpga_mgr(struct xmgmt_main *xmm, void *axlf, size_t size)
{
	int ret;

	WARN_ON(!mutex_is_locked(&xmm->lock));

	/*
	 * Should any error happens during download, we can't trust
	 * the cached xclbin any more.
	 */
	vfree(xmm->firmware_ulp);
	xmm->firmware_ulp = NULL;

	ret = xmgmt_process_xclbin(xmm->xdev, xmm->fmgr, axlf, XMGMT_ULP);
	if (ret == 0)
		xmm->firmware_ulp = axlf;

	return ret;
}

int bitstream_axlf_mailbox(struct xrt_device *xdev, const void *axlf)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);
	void *copy_buffer = NULL;
	size_t copy_buffer_size = 0;
	const struct axlf *xclbin_obj = axlf;
	int ret = 0;

	if (memcmp(xclbin_obj->magic, XCLBIN_VERSION2, sizeof(XCLBIN_VERSION2)))
		return -EINVAL;

	copy_buffer_size = xclbin_obj->header.length;
	if (copy_buffer_size > XCLBIN_MAX_SIZE)
		return -EINVAL;
	copy_buffer = vmalloc(copy_buffer_size);
	if (!copy_buffer)
		return -ENOMEM;
	memcpy(copy_buffer, axlf, copy_buffer_size);

	mutex_lock(&xmm->lock);
	ret = xmgmt_bitstream_axlf_fpga_mgr(xmm, copy_buffer, copy_buffer_size);
	mutex_unlock(&xmm->lock);
	if (ret)
		vfree(copy_buffer);
	return ret;
}

static int bitstream_axlf_ioctl(struct xmgmt_main *xmm, const void __user *arg)
{
	struct xmgmt_ioc_bitstream_axlf ioc_obj = { 0 };
	struct axlf xclbin_obj = { {0} };
	const void __user *xclbin;
	size_t copy_buffer_size = 0;
	void *copy_buffer = NULL;
	int ret = 0;

	if (copy_from_user((void *)&ioc_obj, arg, sizeof(ioc_obj)))
		return -EFAULT;
	xclbin = (const void __user *)ioc_obj.xclbin;
	if (copy_from_user((void *)&xclbin_obj, xclbin, sizeof(xclbin_obj)))
		return -EFAULT;
	if (memcmp(xclbin_obj.magic, XCLBIN_VERSION2, sizeof(XCLBIN_VERSION2)))
		return -EINVAL;

	copy_buffer_size = xclbin_obj.header.length;
	if (copy_buffer_size > XCLBIN_MAX_SIZE || copy_buffer_size < sizeof(xclbin_obj))
		return -EINVAL;
	if (xclbin_obj.header.version_major != XMGMT_SUPP_XCLBIN_MAJOR)
		return -EINVAL;

	copy_buffer = vmalloc(copy_buffer_size);
	if (!copy_buffer)
		return -ENOMEM;

	if (copy_from_user(copy_buffer, xclbin, copy_buffer_size)) {
		vfree(copy_buffer);
		return -EFAULT;
	}

	ret = xmgmt_bitstream_axlf_fpga_mgr(xmm, copy_buffer, copy_buffer_size);
	if (ret)
		vfree(copy_buffer);

	return ret;
}

static long xmgmt_main_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct xmgmt_main *xmm = filp->private_data;
	long result = 0;

	if (_IOC_TYPE(cmd) != XMGMT_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&xmm->lock);

	xrt_info(xmm->xdev, "ioctl cmd %d, arg %ld", cmd, arg);
	switch (cmd) {
	case XMGMT_IOCICAPDOWNLOAD_AXLF:
		result = bitstream_axlf_ioctl(xmm, (const void __user *)arg);
		break;
	default:
		result = -ENOTTY;
		break;
	}

	mutex_unlock(&xmm->lock);
	return result;
}

void *xmgmt_xdev2mailbox(struct xrt_device *xdev)
{
	struct xmgmt_main *xmm = xrt_get_drvdata(xdev);

	return xmm->mailbox_hdl;
}

static struct xrt_dev_endpoints xrt_mgmt_main_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names []){
			{ .ep_name = XRT_MD_NODE_MGMT_MAIN },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_driver xmgmt_main_driver = {
	.driver	= {
		.name = XMGMT_MAIN,
	},
	.file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = xmgmt_main_open,
			.release = xmgmt_main_close,
			.unlocked_ioctl = xmgmt_main_ioctl,
		},
		.xsf_dev_name = "xmgmt",
	},
	.subdev_id = XRT_SUBDEV_MGMT_MAIN,
	.endpoints = xrt_mgmt_main_endpoints,
	.probe = xmgmt_main_probe,
	.remove = xmgmt_main_remove,
	.leaf_call = xmgmt_mainleaf_call,
};

int xmgmt_register_leaf(void)
{
	return xrt_register_driver(&xmgmt_main_driver);
}

void xmgmt_unregister_leaf(void)
{
	xrt_unregister_driver(&xmgmt_main_driver);
}
