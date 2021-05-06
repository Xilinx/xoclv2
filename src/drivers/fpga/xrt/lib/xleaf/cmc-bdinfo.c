// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include "xleaf.h"
#include "xrt-cmc-impl.h"
#include "xmgmt-main.h"
#include <linux/xrt/mailbox_proto.h>

enum board_info_key {
	BDINFO_SN = 0x21,
	BDINFO_MAC0,
	BDINFO_MAC1,
	BDINFO_MAC2,
	BDINFO_MAC3,
	BDINFO_REV,
	BDINFO_NAME,
	BDINFO_BMC_VER,
	BDINFO_MAX_PWR,
	BDINFO_FAN_PRESENCE,
	BDINFO_CONFIG_MODE,
	BDINFO_MAC_DYNAMIC = 0x4b,
};

struct xrt_cmc_bdinfo {
	struct xrt_device *xdev;
	struct mutex lock; /* protect bdinfo */
	char *bdinfo;
	size_t bdinfo_sz;
};

static const char *cmc_parse_board_info(struct xrt_cmc_bdinfo *cmc_bdi,
					enum board_info_key key, size_t *len)
{
	const char *buf = cmc_bdi->bdinfo, *p;
	u32 sz = cmc_bdi->bdinfo_sz;

	WARN_ON(!mutex_is_locked(&cmc_bdi->lock));

	if (!buf)
		return NULL;

	for (p = buf; p < buf + sz;) {
		char k = *(p++);
		u8 l = *(p++);

		if (k == key) {
			if (len)
				*len = l;
			return p;
		}
		p += l;
	}

	return NULL;
}

static int cmc_refresh_board_info_nolock(struct xrt_cmc_bdinfo *cmc_bdi)
{
	int ret = 0;
	int gen = -EINVAL;
	char *bdinfo_raw = NULL;
	size_t bd_info_sz = cmc_mailbox_max_payload(cmc_bdi->xdev);
	struct xrt_device *xdev = cmc_bdi->xdev;
	void *newinfo = NULL;

	WARN_ON(!mutex_is_locked(&cmc_bdi->lock));

	bdinfo_raw = vzalloc(bd_info_sz);
	if (!bdinfo_raw) {
		ret = -ENOMEM;
		goto done;
	}

	/* Load new info from HW. */
	gen = cmc_mailbox_acquire(xdev);
	if (gen < 0) {
		xrt_err(xdev, "failed to hold mailbox: %d", gen);
		ret = gen;
		goto done;
	}
	ret = cmc_mailbox_send_packet(xdev, gen, CMC_MBX_PKT_OP_BOARD_INFO, NULL, 0);
	if (ret) {
		xrt_err(xdev, "failed to send pkt: %d", ret);
		goto done;
	}
	ret = cmc_mailbox_recv_packet(xdev, gen, bdinfo_raw, &bd_info_sz);
	if (ret) {
		xrt_err(xdev, "failed to receive pkt: %d", ret);
		goto done;
	}

	newinfo = kmemdup(bdinfo_raw, bd_info_sz, GFP_KERNEL);
	if (!newinfo) {
		ret = -ENOMEM;
		goto done;
	}

	kfree(cmc_bdi->bdinfo);
	cmc_bdi->bdinfo = newinfo;
	cmc_bdi->bdinfo_sz = bd_info_sz;

done:
	if (gen >= 0)
		cmc_mailbox_release(xdev, gen);
	vfree(bdinfo_raw);
	return ret;
}

int cmc_refresh_board_info(struct xrt_device *xdev)
{
	int ret;
	struct xrt_cmc_bdinfo *cmc_bdi = cmc_xdev2bdinfo(xdev);

	if (!cmc_bdi)
		return -ENODEV;

	mutex_lock(&cmc_bdi->lock);
	ret = cmc_refresh_board_info_nolock(cmc_bdi);
	mutex_unlock(&cmc_bdi->lock);
	return ret;
}

static void cmc_copy_board_info_by_key(struct xrt_cmc_bdinfo *cmc_bdi,
				       enum board_info_key key, void *target)
{
	size_t len;
	const char *info;

	info = cmc_parse_board_info(cmc_bdi, key, &len);
	if (!info)
		return;
	memcpy(target, info, len);
}

static void cmc_copy_dynamic_mac(struct xrt_cmc_bdinfo *cmc_bdi, u32 *num_mac, void *first_mac)
{
	size_t len = 0;
	const char *info;
	u16 num = 0;

	info = cmc_parse_board_info(cmc_bdi, BDINFO_MAC_DYNAMIC, &len);
	if (!info)
		return;

	if (len != 8) {
		xrt_err(cmc_bdi->xdev, "dynamic mac data is corrupted.");
		return;
	}

	/*
	 * Byte 0:1 is contiguous mac addresses number in LSB.
	 * Byte 2:7 is first mac address.
	 */
	memcpy(&num, info, 2);
	*num_mac = le16_to_cpu(num);
	memcpy(first_mac, info + 2, 6);
}

static void cmc_copy_expect_bmc(struct xrt_cmc_bdinfo *cmc_bdi, void *expbmc)
{
/* Not a real SC version to indicate that SC image does not exist. */
#define NONE_BMC_VERSION	"0.0.0"
	int ret = 0;
	struct xrt_device *xdev = cmc_bdi->xdev;
	struct xrt_device *mgmt_leaf =
		xleaf_get_leaf_by_id(xdev, XRT_SUBDEV_MGMT_MAIN, XRT_INVALID_DEVICE_INST);
	struct xrt_mgmt_main_get_axlf_section gs = { XMGMT_BLP, BMC, };
	struct bmc *bmcsect;

	(void)sprintf(expbmc, "%s", NONE_BMC_VERSION);

	if (!mgmt_leaf) {
		xrt_err(xdev, "failed to get hold of main");
		return;
	}

	ret = xleaf_call(mgmt_leaf, XRT_MGMT_MAIN_GET_AXLF_SECTION, &gs);
	if (ret == 0) {
		bmcsect = (struct bmc *)gs.xmmigas_section;
		memcpy(expbmc, bmcsect->version, sizeof(bmcsect->version));
	} else {
		/*
		 * no SC section, SC should be fixed, expected SC should be
		 * the same as on board SC.
		 */
		cmc_copy_board_info_by_key(cmc_bdi, BDINFO_BMC_VER, expbmc);
	}
	xleaf_put_leaf(xdev, mgmt_leaf);
}

int cmc_bdinfo_read(struct xrt_device *xdev, struct xcl_board_info *bdinfo)
{
	struct xrt_cmc_bdinfo *cmc_bdi = cmc_xdev2bdinfo(xdev);

	mutex_lock(&cmc_bdi->lock);

	if (!cmc_bdi->bdinfo) {
		xrt_err(cmc_bdi->xdev, "board info is not available");
		mutex_unlock(&cmc_bdi->lock);
		return -ENOENT;
	}

	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_SN, bdinfo->serial_num);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_MAC0, bdinfo->mac_addr0);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_MAC1, bdinfo->mac_addr1);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_MAC2, bdinfo->mac_addr2);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_MAC3, bdinfo->mac_addr3);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_REV, bdinfo->revision);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_NAME, bdinfo->bd_name);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_BMC_VER, bdinfo->bmc_ver);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_MAX_PWR, &bdinfo->max_power);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_FAN_PRESENCE, &bdinfo->fan_presence);
	cmc_copy_board_info_by_key(cmc_bdi, BDINFO_CONFIG_MODE, &bdinfo->config_mode);
	cmc_copy_dynamic_mac(cmc_bdi, &bdinfo->mac_contiguous_num, bdinfo->mac_addr_first);
	cmc_copy_expect_bmc(cmc_bdi, bdinfo->exp_bmc_ver);

	mutex_unlock(&cmc_bdi->lock);
	return 0;
}

#define CMC_BDINFO_STRING_SYSFS_NODE(name, key)				\
	static ssize_t name##_show(struct device *dev,			\
		struct device_attribute *attr, char *buf)		\
	{								\
		const char *s;						\
		struct xrt_device *xdev = to_xrt_dev(dev);		\
		struct xrt_cmc_bdinfo *cmc_bdi = cmc_xdev2bdinfo(xdev);\
									\
		mutex_lock(&cmc_bdi->lock);				\
		s = cmc_parse_board_info(cmc_bdi, key, NULL);		\
		mutex_unlock(&cmc_bdi->lock);				\
		return sprintf(buf, "%s\n", s ? s : "");		\
	}								\
	static DEVICE_ATTR_RO(name)

CMC_BDINFO_STRING_SYSFS_NODE(bd_name, BDINFO_NAME);
CMC_BDINFO_STRING_SYSFS_NODE(bmc_ver, BDINFO_BMC_VER);

static struct attribute *cmc_bdinfo_attrs[] = {
	&dev_attr_bd_name.attr,
	&dev_attr_bmc_ver.attr,
	NULL
};

static ssize_t bdinfo_raw_show(struct file *filp, struct kobject *kobj,
			       struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	ssize_t ret = 0;
	struct device *dev = container_of(kobj, struct device, kobj);
	struct xrt_device *xdev = to_xrt_dev(dev);
	struct xrt_cmc_bdinfo *cmc_bdi = cmc_xdev2bdinfo(xdev);

	if (!cmc_bdi || !cmc_bdi->bdinfo_sz)
		return 0;

	mutex_lock(&cmc_bdi->lock);

	if (off < cmc_bdi->bdinfo_sz) {
		if (off + count > cmc_bdi->bdinfo_sz)
			count = cmc_bdi->bdinfo_sz - off;
		memcpy(buf, cmc_bdi->bdinfo + off, count);
		ret = count;
	}

	mutex_unlock(&cmc_bdi->lock);
	return ret;
}

static struct bin_attribute bdinfo_raw_attr = {
	.attr = {
		.name = "board_info_raw",
		.mode = 0400
	},
	.read = bdinfo_raw_show,
	.size = 0
};

static struct bin_attribute *cmc_bdinfo_bin_attrs[] = {
	&bdinfo_raw_attr,
	NULL,
};

static struct attribute_group cmc_bdinfo_attr_group = {
	.attrs = cmc_bdinfo_attrs,
	.bin_attrs = cmc_bdinfo_bin_attrs,
};

void cmc_bdinfo_remove(struct xrt_device *xdev)
{
	struct xrt_cmc_bdinfo *cmc_bdi = cmc_xdev2bdinfo(xdev);

	if (!cmc_bdi)
		return;

	sysfs_remove_group(&xdev->dev.kobj, &cmc_bdinfo_attr_group);
	kfree(cmc_bdi->bdinfo);
}

int cmc_bdinfo_probe(struct xrt_device *xdev, struct cmc_reg_map *regmaps, void **hdl)
{
	int ret;
	struct xrt_cmc_bdinfo *cmc_bdi;

	cmc_bdi = devm_kzalloc(DEV(xdev), sizeof(*cmc_bdi), GFP_KERNEL);
	if (!cmc_bdi)
		return -ENOMEM;

	cmc_bdi->xdev = xdev;
	mutex_init(&cmc_bdi->lock);

	mutex_lock(&cmc_bdi->lock);
	ret = cmc_refresh_board_info_nolock(cmc_bdi);
	mutex_unlock(&cmc_bdi->lock);
	if (ret) {
		xrt_err(xdev, "failed to load board info: %d", ret);
		goto fail;
	}

	ret = sysfs_create_group(&xdev->dev.kobj, &cmc_bdinfo_attr_group);
	if (ret) {
		xrt_err(xdev, "create bdinfo attrs failed: %d", ret);
		goto fail;
	}

	*hdl = cmc_bdi;
	return 0;

fail:
	cmc_bdinfo_remove(xdev);
	devm_kfree(DEV(xdev), cmc_bdi);
	return ret;
}
