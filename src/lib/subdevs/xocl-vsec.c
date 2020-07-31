// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "xocl-parent.h"

#define XOCL_VSEC "xocl_vsec"

#define VSEC_TYPE_UUID		0x50
#define VSEC_TYPE_FLASH		0x51
#define VSEC_TYPE_PLATINFO	0x52
#define VSEC_TYPE_MAILBOX	0x53

struct xocl_vsec_header {
	u32		format;
	u32		length;
	u8		rsvd0[3];
	u8		entry_sz;
	u32		rsvd;
};

#define GET_BAR(entry)	((entry->bar_rev >> 4) & 0xf)
#define GET_BAR_OFF(entry)	(entry->off_lo | ((u64)entry->off_hi << 16))

struct xocl_vsec_entry {
	u16		off_lo;
	u8		bar_rev;
	u8		type;
	u32		off_hi;
	u8		rsvd0;
	u8		major;
	u8		minor;
	u8		ver_type;
	u32		rsvd1;
};

struct vsec_device {
	u8		type;
	char		*ep_name;
	ulong		size;
};

struct vsec_device vsec_devs[] = {
	{
		.type = VSEC_TYPE_UUID,
		.ep_name = NODE_BLP_ROM,
		.size = 16,
	},
	{
		.type = VSEC_TYPE_FLASH,
		.ep_name = NODE_FLASH,
		.size = 4096,
	},
	{
		.type = VSEC_TYPE_PLATINFO,
		.ep_name = NODE_PLAT_INFO,
		.size = 4,
	},
	{
		.type = VSEC_TYPE_MAILBOX,
		.ep_name = NODE_MAILBOX_MGMT,
		.size = 48,
	},
};

struct xocl_vsec {
	struct platform_device	*pdev;
	void			*base;
	ulong			length;

	char			*metadata;
};

static int xocl_vsec_add_node(struct xocl_vsec *vsec,
	void *md_blob, char *ep_name, int bar, u64 bar_off, ulong size)
{
	struct xocl_md_endpoint ep;
	int ret;

	ep.ep_name = ep_name;
	ep.bar = bar;
	ep.bar_off = bar_off;
	ep.size = size;
	ret = xocl_md_add_endpoint(DEV(vsec->pdev), &vsec->metadata, &ep);
	if (ret) {
		xocl_err(vsec->pdev, "add ep failed, ret %d", ret);
		goto failed;
	}

failed:
	return ret;
}

static char *type2epname(u32 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vsec_devs); i++) {
		if (vsec_devs[i].type == type)
			return (vsec_devs[i].ep_name);
	}

	return NULL;
}

static ulong type2size(u32 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vsec_devs); i++) {
		if (vsec_devs[i].type == type)
			return (vsec_devs[i].size);
	}

	return 0;
}

static int xocl_vsec_create_metadata(struct xocl_vsec *vsec)
{
	struct xocl_vsec_entry *p_entry;
	u64 offset;
	int ret;

	ret = xocl_md_create(&vsec->pdev->dev, &vsec->metadata);
	if (ret) {
		xocl_err(vsec->pdev, "create metadata failed");
		return ret;
	}

	for (p_entry = vsec->base + sizeof(struct xocl_vsec_header);
	    (ulong)p_entry < (ulong)vsec->base + vsec->length;
	    p_entry++) {
		offset = (((u64)p_entry->off_hi) << 16) | p_entry->off_lo;
		ret = xocl_vsec_add_node(vsec, vsec->metadata,
			type2epname(p_entry->type), GET_BAR(p_entry),
			GET_BAR_OFF(p_entry), type2size(p_entry->type));
	}

	return 0;
}

static int xocl_vsec_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	return 0;
}

static ssize_t metadata_output(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct xocl_vsec *vsec = dev_get_drvdata(dev);
	unsigned char *blob;
	size_t size;
	ssize_t ret = 0;

	if (!vsec->metadata)
		goto failed;

	blob = vsec->metadata;
	size = xocl_md_size(dev, blob);

	if (off >= size)
		goto failed;

	if (off + count > size)
		count = size - off;
	memcpy(buf, blob + off, count);

	ret = count;
failed:
	return ret;
}

static struct bin_attribute meta_data_attr = {
	.attr = {
		.name = "metadata",
		.mode = 0400
	},
	.read = metadata_output,
	.size = 0
};

static struct bin_attribute  *vsec_bin_attrs[] = {
	&meta_data_attr,
	NULL,
};

static struct attribute_group  vsec_attr_group = {
	.bin_attrs = vsec_bin_attrs,
};

static int xocl_vsec_mapio(struct xocl_vsec *vsec)
{
	struct xocl_subdev_platdata *pdata = DEV_PDATA(vsec->pdev);
	struct xocl_vsec_header *p_hdr;
	ulong length;

	if (!pdata || !pdata->xsp_dtb) {
		xocl_err(vsec->pdev, "empty metadata");
		return -EINVAL;
	}

	p_hdr = ioremap(0x1f8000, sizeof(*p_hdr));
	if (!p_hdr) {
		xocl_err(vsec->pdev, "Map header failed");
		return -EIO;
	}

	length = p_hdr->length;
	iounmap(p_hdr);
	vsec->base = ioremap(0x1f8000, length);
	if (!vsec->base) {
		xocl_err(vsec->pdev, "map failed");
		return -EIO;
	}

	return 0;
}

static int xocl_vsec_remove(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;

	vsec = platform_get_drvdata(pdev);

	if (vsec->base) {
		iounmap(vsec->base);
		vsec->base = NULL;
	}

	return 0;
}

static int xocl_vsec_probe(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;
	int			ret = 0;

	vsec = devm_kzalloc(&pdev->dev, sizeof(*vsec), GFP_KERNEL);
	if (!vsec)
		return -ENOMEM;

	vsec->pdev = pdev;
	platform_set_drvdata(pdev, vsec);

	ret = xocl_vsec_mapio(vsec);
	if (ret)
		goto failed;

	ret = xocl_vsec_create_metadata(vsec);
	if (ret) {
		xocl_err(pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}
	ret = xocl_subdev_create_partition(pdev, vsec->metadata);
	if (ret < 0)
		xocl_err(pdev, "create partition failed, ret %d", ret);

	ret = sysfs_create_group(&DEV(pdev)->kobj, &vsec_attr_group);
	if (ret)
		xocl_err(pdev, "create sysfs failed, ret %d", ret);

failed:
	if (ret)
		xocl_vsec_remove(pdev);

	return ret;
}

struct xocl_subdev_endpoints xocl_vsec_endpoints = {
	.xse_names = (struct xocl_subdev_ep_names []){
		{ .ep_name = NODE_VSEC },
		{ NULL },
	},
	.xse_min_ep = 1,
};

struct xocl_subdev_drvdata xocl_vsec_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_vsec_ioctl,
	},
};

static const struct platform_device_id xocl_vsec_table[] = {
	{ XOCL_VSEC, (kernel_ulong_t)&xocl_vsec_data },
	{ },
};

struct platform_driver xocl_vsec_driver = {
	.driver = {
		.name = XOCL_VSEC,
	},
	.probe = xocl_vsec_probe,
	.remove = xocl_vsec_remove,
	.id_table = xocl_vsec_table,
};
