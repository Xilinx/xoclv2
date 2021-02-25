// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA VSEC Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

#include <linux/platform_device.h>
#include "metadata.h"
#include "xleaf.h"

#define XRT_VSEC "xrt_vsec"

#define VSEC_TYPE_UUID		0x50
#define VSEC_TYPE_FLASH		0x51
#define VSEC_TYPE_PLATINFO	0x52
#define VSEC_TYPE_MAILBOX	0x53
#define VSEC_TYPE_END		0xff

#define VSEC_UUID_LEN		16

struct xrt_vsec_header {
	u32		format;
	u32		length;
	u32		entry_sz;
	u32		rsvd;
} __packed;

#define head_rd(g, r)			\
	ioread32((void *)(g)->base + offsetof(struct xrt_vsec_header, r))

#define GET_BAR(entry)	(((entry)->bar_rev >> 4) & 0xf)
#define GET_BAR_OFF(_entry)				\
	({ typeof(_entry) entry = (_entry);		\
	 ((entry)->off_lo | ((u64)(entry)->off_hi << 16)); })
#define GET_REV(entry)	((entry)->bar_rev & 0xf)

struct xrt_vsec_entry {
	u8		type;
	u8		bar_rev;
	u16		off_lo;
	u32		off_hi;
	u8		ver_type;
	u8		minor;
	u8		major;
	u8		rsvd0;
	u32		rsvd1;
} __packed;

#define read_entry(g, i, e)					\
	do {							\
		u32 *p = (u32 *)((g)->base +			\
			sizeof(struct xrt_vsec_header) +	\
			(i) * sizeof(struct xrt_vsec_entry));	\
		u32 off;					\
		for (off = 0;					\
		    off < sizeof(struct xrt_vsec_entry) / 4;	\
		    off++)					\
			*((u32 *)(e) + off) = ioread32(p + off);\
	} while (0)

struct vsec_device {
	u8		type;
	char		*ep_name;
	ulong		size;
	char		*regmap;
};

static struct vsec_device vsec_devs[] = {
	{
		.type = VSEC_TYPE_UUID,
		.ep_name = XRT_MD_NODE_BLP_ROM,
		.size = VSEC_UUID_LEN,
		.regmap = "vsec-uuid",
	},
	{
		.type = VSEC_TYPE_FLASH,
		.ep_name = XRT_MD_NODE_FLASH_VSEC,
		.size = 4096,
		.regmap = "vsec-flash",
	},
	{
		.type = VSEC_TYPE_PLATINFO,
		.ep_name = XRT_MD_NODE_PLAT_INFO,
		.size = 4,
		.regmap = "vsec-platinfo",
	},
	{
		.type = VSEC_TYPE_MAILBOX,
		.ep_name = XRT_MD_NODE_MAILBOX_VSEC,
		.size = 48,
		.regmap = "vsec-mbx",
	},
};

struct xrt_vsec {
	struct platform_device	*pdev;
	void			*base;
	ulong			length;

	char			*metadata;
	char			uuid[VSEC_UUID_LEN];
};

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

static char *type2regmap(u32 type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vsec_devs); i++) {
		if (vsec_devs[i].type == type)
			return (vsec_devs[i].regmap);
	}

	return NULL;
}

static int xrt_vsec_add_node(struct xrt_vsec *vsec,
			     void *md_blob, struct xrt_vsec_entry *p_entry)
{
	struct xrt_md_endpoint ep;
	char regmap_ver[64];
	int ret;

	if (!type2epname(p_entry->type))
		return -EINVAL;

	/*
	 * VSEC may have more than 1 mailbox instance for the card
	 * which has more than 1 physical function.
	 * This is not supported for now. Assuming only one mailbox
	 */

	snprintf(regmap_ver, sizeof(regmap_ver) - 1, "%d-%d.%d.%d",
		 p_entry->ver_type, p_entry->major, p_entry->minor,
		 GET_REV(p_entry));
	ep.ep_name = type2epname(p_entry->type);
	ep.bar = GET_BAR(p_entry);
	ep.bar_off = GET_BAR_OFF(p_entry);
	ep.size = type2size(p_entry->type);
	ep.regmap = type2regmap(p_entry->type);
	ep.regmap_ver = regmap_ver;
	ret = xrt_md_add_endpoint(DEV(vsec->pdev), vsec->metadata, &ep);
	if (ret) {
		xrt_err(vsec->pdev, "add ep failed, ret %d", ret);
		goto failed;
	}

failed:
	return ret;
}

static int xrt_vsec_create_metadata(struct xrt_vsec *vsec)
{
	struct xrt_vsec_entry entry;
	int i, ret;

	ret = xrt_md_create(&vsec->pdev->dev, &vsec->metadata);
	if (ret) {
		xrt_err(vsec->pdev, "create metadata failed");
		return ret;
	}

	for (i = 0; i * sizeof(entry) < vsec->length -
	    sizeof(struct xrt_vsec_header); i++) {
		read_entry(vsec, i, &entry);
		xrt_vsec_add_node(vsec, vsec->metadata, &entry);
	}

	return 0;
}

static int xrt_vsec_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	int ret = 0;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	default:
		ret = -EINVAL;
		xrt_err(pdev, "should never been called");
		break;
	}

	return ret;
}

static int xrt_vsec_mapio(struct xrt_vsec *vsec)
{
	struct xrt_subdev_platdata *pdata = DEV_PDATA(vsec->pdev);
	const u32 *bar;
	const u64 *bar_off;
	struct resource *res = NULL;
	ulong addr;
	int ret;

	if (!pdata || xrt_md_size(DEV(vsec->pdev), pdata->xsp_dtb) == XRT_MD_INVALID_LENGTH) {
		xrt_err(vsec->pdev, "empty metadata");
		return -EINVAL;
	}

	ret = xrt_md_get_prop(DEV(vsec->pdev), pdata->xsp_dtb, XRT_MD_NODE_VSEC,
			      NULL, XRT_MD_PROP_BAR_IDX, (const void **)&bar, NULL);
	if (ret) {
		xrt_err(vsec->pdev, "failed to get bar idx, ret %d", ret);
		return -EINVAL;
	}

	ret = xrt_md_get_prop(DEV(vsec->pdev), pdata->xsp_dtb, XRT_MD_NODE_VSEC,
			      NULL, XRT_MD_PROP_OFFSET, (const void **)&bar_off, NULL);
	if (ret) {
		xrt_err(vsec->pdev, "failed to get bar off, ret %d", ret);
		return -EINVAL;
	}

	xrt_info(vsec->pdev, "Map vsec at bar %d, offset 0x%llx",
		 be32_to_cpu(*bar), be64_to_cpu(*bar_off));

	xleaf_get_barres(vsec->pdev, &res, be32_to_cpu(*bar));
	if (!res) {
		xrt_err(vsec->pdev, "failed to get bar addr");
		return -EINVAL;
	}

	addr = res->start + (ulong)be64_to_cpu(*bar_off);

	vsec->base = ioremap(addr, sizeof(struct xrt_vsec_header));
	if (!vsec->base) {
		xrt_err(vsec->pdev, "Map header failed");
		return -EIO;
	}

	vsec->length = head_rd(vsec, length);
	iounmap(vsec->base);
	vsec->base = ioremap(addr, vsec->length);
	if (!vsec->base) {
		xrt_err(vsec->pdev, "map failed");
		return -EIO;
	}

	return 0;
}

static int xrt_vsec_remove(struct platform_device *pdev)
{
	struct xrt_vsec	*vsec;

	vsec = platform_get_drvdata(pdev);

	if (vsec->base) {
		iounmap(vsec->base);
		vsec->base = NULL;
	}

	vfree(vsec->metadata);

	return 0;
}

static int xrt_vsec_probe(struct platform_device *pdev)
{
	struct xrt_vsec	*vsec;
	int			ret = 0;

	vsec = devm_kzalloc(&pdev->dev, sizeof(*vsec), GFP_KERNEL);
	if (!vsec)
		return -ENOMEM;

	vsec->pdev = pdev;
	platform_set_drvdata(pdev, vsec);

	ret = xrt_vsec_mapio(vsec);
	if (ret)
		goto failed;

	ret = xrt_vsec_create_metadata(vsec);
	if (ret) {
		xrt_err(pdev, "create metadata failed, ret %d", ret);
		goto failed;
	}
	ret = xleaf_create_group(pdev, vsec->metadata);
	if (ret < 0)
		xrt_err(pdev, "create group failed, ret %d", ret);
	else
		ret = 0;

failed:
	if (ret)
		xrt_vsec_remove(pdev);

	return ret;
}

static struct xrt_subdev_endpoints xrt_vsec_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []){
			{ .ep_name = XRT_MD_NODE_VSEC },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_subdev_drvdata xrt_vsec_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_vsec_ioctl,
	},
};

static const struct platform_device_id xrt_vsec_table[] = {
	{ XRT_VSEC, (kernel_ulong_t)&xrt_vsec_data },
	{ },
};

static struct platform_driver xrt_vsec_driver = {
	.driver = {
		.name = XRT_VSEC,
	},
	.probe = xrt_vsec_probe,
	.remove = xrt_vsec_remove,
	.id_table = xrt_vsec_table,
};

void vsec_leaf_init_fini(bool init)
{
	if (init)
		xleaf_register_driver(XRT_SUBDEV_VSEC, &xrt_vsec_driver, xrt_vsec_endpoints);
	else
		xleaf_unregister_driver(XRT_SUBDEV_VSEC);
}
