// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include "xocl-subdev.h"
#include "xocl-cmc-impl.h"

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
};

struct xocl_cmc_bdinfo {
	struct platform_device *pdev;
	struct mutex lock;
	char *bdinfo;
	size_t bdinfo_sz;
};

static const char *cmc_get_board_info(struct xocl_cmc_bdinfo *cmc_bdi,
	enum board_info_key key, size_t *len)
{
	const char *buf = cmc_bdi->bdinfo, *p;
	u32 sz = cmc_bdi->bdinfo_sz;

	BUG_ON(!mutex_is_locked(&cmc_bdi->lock));

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

static int cmc_refresh_board_info_nolock(struct xocl_cmc_bdinfo *cmc_bdi)
{
	int ret = 0;
	int gen = -EINVAL;
	char *bdinfo_raw = NULL;
	size_t bd_info_sz = cmc_mailbox_max_payload(cmc_bdi->pdev);
	struct platform_device *pdev = cmc_bdi->pdev;
	void *newinfo = NULL;

	BUG_ON(!mutex_is_locked(&cmc_bdi->lock));

	bdinfo_raw = vzalloc(bd_info_sz);
	if (bdinfo_raw == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	/* Load new info from HW. */
	gen = cmc_mailbox_acquire(pdev);
	if (gen < 0) {
		xocl_err(pdev, "failed to hold mailbox: %d", gen);
		ret = gen;
		goto done;
	}
	ret = cmc_mailbox_send_packet(pdev, gen, CMC_MBX_PKT_OP_BOARD_INFO,
		NULL, 0);
	if (ret) {
		xocl_err(pdev, "failed to send pkt: %d", ret);
		goto done;
	}
	ret = cmc_mailbox_recv_packet(pdev, gen, bdinfo_raw, &bd_info_sz);
	if (ret) {
		xocl_err(pdev, "failed to receive pkt: %d", ret);
		goto done;
	}

	newinfo = kmemdup(bdinfo_raw, bd_info_sz, GFP_KERNEL);
	if (newinfo == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	kfree(cmc_bdi->bdinfo);
	cmc_bdi->bdinfo = newinfo;
	cmc_bdi->bdinfo_sz = bd_info_sz;

done:
	if (gen >= 0)
		cmc_mailbox_release(pdev, gen);
	vfree(bdinfo_raw);
	return ret;
}

int cmc_refresh_board_info(struct platform_device *pdev)
{
	int ret;
	struct xocl_cmc_bdinfo *cmc_bdi = cmc_pdev2bdinfo(pdev);

	if (!cmc_bdi)
		return -ENODEV;

	mutex_lock(&cmc_bdi->lock);
	ret = cmc_refresh_board_info_nolock(cmc_bdi);
	mutex_unlock(&cmc_bdi->lock);
	return ret;
}

#define	CMC_BDINFO_STRING_SYSFS_NODE(name, key)				\
	static ssize_t name##_show(struct device *dev,			\
		struct device_attribute *attr, char *buf)		\
	{								\
		const char *s;						\
		struct platform_device *pdev = to_platform_device(dev);	\
		struct xocl_cmc_bdinfo *cmc_bdi = cmc_pdev2bdinfo(pdev);\
									\
		mutex_lock(&cmc_bdi->lock);				\
		s = cmc_get_board_info(cmc_bdi, key, NULL);		\
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
	struct platform_device *pdev = to_platform_device(dev);
	struct xocl_cmc_bdinfo *cmc_bdi = cmc_pdev2bdinfo(pdev);

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

void cmc_bdinfo_remove(struct platform_device *pdev)
{
	struct xocl_cmc_bdinfo *cmc_bdi = cmc_pdev2bdinfo(pdev);

	if (!cmc_bdi)
		return;

	sysfs_remove_group(&pdev->dev.kobj, &cmc_bdinfo_attr_group);
	kfree(cmc_bdi->bdinfo);
}

int cmc_bdinfo_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl)
{
	int ret;
	struct xocl_cmc_bdinfo *cmc_bdi;

	cmc_bdi = devm_kzalloc(DEV(pdev), sizeof(*cmc_bdi), GFP_KERNEL);
	if (!cmc_bdi)
		return -ENOMEM;

	cmc_bdi->pdev = pdev;
	mutex_init(&cmc_bdi->lock);

	mutex_lock(&cmc_bdi->lock);
	ret = cmc_refresh_board_info_nolock(cmc_bdi);
	mutex_unlock(&cmc_bdi->lock);
	if (ret) {
		xocl_err(pdev, "failed to load board info: %d", ret);
		goto fail;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &cmc_bdinfo_attr_group);
	if (ret) {
		xocl_err(pdev, "create bdinfo attrs failed: %d", ret);
		goto fail;
	}

	*hdl = cmc_bdi;
	return 0;

fail:
	cmc_bdinfo_remove(pdev);
	devm_kfree(DEV(pdev), cmc_bdi);
	return ret;
}
