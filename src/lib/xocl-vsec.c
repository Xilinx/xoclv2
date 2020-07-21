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
#include "xocl-subdev.h"

#define XOCL_VSEC "xocl_vsec"

#define VSEC_TYPE_UUID		0x50
#define VSEC_TYPE_FLASH		0x51
#define VSEC_TYPE_PLATINFO	0x52
#define VSEC_TYPE_MAILBOX	0x53

struct xocl_vsec_header {
	u32		format;
	u32		length;
	u8		rsvd0[3]
	u8		entry_sz;
	u32		rsvd;
};

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

struct vsec_type_name {
	u8		type;
	char		*ep_name;
};

struct vsec_type_name type_name_map[] = {
	{
		.type = VSEC_TYPE_UUID;
		.ep_name = NODE_BLP_ROM;
	},
	{
		.type = VSEC_TYPE_FLASH;
		.ep_name = NODE_FLASH;
	},
	{
		.type = VSEC_TYPE_PLATINFO;
		.ep_name = NODE_PLAT_INFO;
	},
	{
		.type = VSEC_TYPE_MAILBOX;
		.ep_name = NODE_MAILBOX_MGMT;
	},
};

struct xocl_vsec {
	struct platform_device	*pdev;
	void			*base;
};

static int xocl_vsec_add_node(struct xocl_vsec *vsec,
	void *md_blob, *ep_name, int bar, ulong bar_off, ulong size)
{
	return 0;
}

static char *type2epname(u32 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(type_name_map); i++) {
		if (type_name_map[i].type == type)
			return (type_name_map[i].ep_name);
	}

	return NULL;
}

static int xocl_vsec_create_metadata(struct xocl_vsec *vsec)
{
	struct xocl_vsec_header *p_hdr;
	struct xocl_vsec_entry *p_entry;
	u64 offset;
	int length, i;

	p_hdr = vsec->base;
	length = p_hdr->length;

	for(p_entry = vsec->base + sizeof(*p_hdr);
	    p_entry < vsec->base + length;
	    p_entry++) {
		offset = (((u64)p_entry->off_hi) << 16) | p_entry->off_lo;
	}
}

static long xocl_vsec_ioctl(struct platform_device *pdev, u32 cmd, u64 arg)
{
	return 0;
}

static int xocl_vsec_remove(struct platform_device *pdev)
{
	struct xocl_vsec	*vsec;

	vsec = platform_get_drvdata(pdev);


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

failed:
	if (ret)
		xocl_vsec_remove(pdev);

	return ret;
}

struct xocl_subdev_data xocl_vsec_data = {
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
