// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA Metadata parse APIs
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#include <linux/libfdt_env.h>
#include "libfdt.h"
#include "xocl-metadata.h"

#define BLOB_DELTA	4096

#define md_err(dev, fmt, args...)			\
	dev_err(dev, "%s: "fmt, __func__, ##args)
#define md_warn(dev, fmt, args...)			\
	dev_warn(dev, "%s: "fmt, __func__, ##args)
#define md_info(dev, fmt, args...)			\
	dev_info(dev, "%s: "fmt, __func__, ##args)
#define md_dbg(dev, fmt, args...)			\
	dev_dbg(dev, "%s: "fmt, __func__, ##args)

static int xocl_md_setprop(struct device *dev, char **blob, int offset,
	const char *prop, const void *val, int size);
static int xocl_md_overlay(struct device *dev, char **blob, int target,
	char *overlay_blob, int overlay_offset);
static int xocl_md_get_endpoint(struct device *dev, char *blob,
	const char *ep_name, char *regmap_name, int *ep_offset);

long xocl_md_size(struct device *dev, char *blob)
{
	return (long) fdt_totalsize(blob);
}

int xocl_md_create(struct device *dev, char **blob)
{
	int ret = 0;

	WARN_ON(!blob);

	*blob = vmalloc(BLOB_DELTA);
	if (!*blob)
		return -ENOMEM;

	ret = fdt_create_empty_tree(*blob, BLOB_DELTA);
	if (ret) {
		md_err(dev, "format blob failed, ret = %d", ret);
		goto failed;
	}

	ret = fdt_next_node(*blob, -1, NULL);
	if (ret < 0) {
		md_err(dev, "No Node, ret = %d", ret);
		goto failed;
	}

	ret = fdt_add_subnode(*blob, ret, NODE_ENDPOINTS);
	if (ret < 0)
		md_err(dev, "add node failed, ret = %d", ret);

failed:
	if (ret < 0) {
		vfree(*blob);
		*blob = NULL;
	} else
		ret = 0;

	return ret;
}

int xocl_md_add_node(struct device *dev, char **blob, int parent_offset,
	const char *ep_name)
{
	int ret;
	char *buf = NULL;
	int bufsz;

	ret = fdt_add_subnode(*blob, parent_offset, ep_name);
	if (ret == -FDT_ERR_NOSPACE) {
		bufsz = fdt_totalsize(*blob) + BLOB_DELTA;
		buf = vmalloc(bufsz);
		if (!buf)
			return -ENOMEM;

		ret = fdt_resize(*blob, buf, bufsz);
		if (ret) {
			md_err(dev, "resize failed, ret = %d", ret);
			goto failed;
		}
		ret = fdt_add_subnode(*blob, parent_offset, ep_name);
		if (ret < 0) {
			md_err(dev, "add node failed, ret = %d", ret);
			goto failed;
		}
		vfree(*blob);
		*blob = buf;
		buf = NULL;
	}

failed:
	if (ret < 0 && buf)
		vfree(buf);

	return ret;
}

int xocl_md_del_endpoint(struct device *dev, char **blob, const char *ep_name,
	char *regmap_name)
{
	int ret;
	int ep_offset;

	ret = xocl_md_get_endpoint(dev, *blob, ep_name, regmap_name, &ep_offset);
	if (ret) {
		md_err(dev, "can not find ep %s", ep_name);
		return -EINVAL;
	}

	ret = fdt_del_node(*blob, ep_offset);
	if (ret)
		md_err(dev, "delete node %s failed, ret %d", ep_name, ret);

	return ret;
}

static int __xocl_md_add_endpoint(struct device *dev, char **blob,
	struct xocl_md_endpoint *ep, int *offset)
{
	int ret = 0;
	int ep_offset;
	u32 val, count = 0;
	u64 io_range[2];
	char comp[128];

	if (!ep->ep_name) {
		md_err(dev, "empty name");
		return -EINVAL;
	}

	ret = xocl_md_get_endpoint(dev, *blob, NODE_ENDPOINTS, NULL, &ep_offset);
	if (ret) {
		md_err(dev, "invalid blob, ret = %d", ret);
		return -EINVAL;
	}

	ep_offset = xocl_md_add_node(dev, blob, ep_offset, ep->ep_name);
	if (ep_offset < 0) {
		md_err(dev, "add endpoint failed, ret = %d", ret);
		return -EINVAL;
	}
	if (offset)
		*offset = ep_offset;

	if (ep->size != 0) {
		val = cpu_to_be32(ep->bar);
		ret = xocl_md_setprop(dev, blob, ep_offset, PROP_BAR_IDX,
				&val, sizeof(u32));
		if (ret) {
			md_err(dev, "set %s failed, ret %d",
				PROP_BAR_IDX, ret);
			goto failed;
		}
		io_range[0] = cpu_to_be64((u64)ep->bar_off);
		io_range[1] = cpu_to_be64((u64)ep->size);
		ret = xocl_md_setprop(dev, blob, ep_offset, PROP_IO_OFFSET,
			io_range, sizeof(io_range));
		if (ret) {
			md_err(dev, "set %s failed, ret %d",
				PROP_IO_OFFSET, ret);
			goto failed;
		}
	}

	if (ep->regmap) {
		if (ep->regmap_ver) {
			count = snprintf(comp, sizeof(comp),
				"%s-%s", ep->regmap, ep->regmap_ver);
			count++;
		}

		count += snprintf(comp + count, sizeof(comp) - count,
			"%s", ep->regmap);
		count++;

		ret = xocl_md_setprop(dev, blob, ep_offset, PROP_COMPATIBLE,
			comp, count);
		if (ret) {
			md_err(dev, "set %s failed, ret %d",
				PROP_COMPATIBLE, ret);
			goto failed;
		}
	}

failed:
	if (ret)
		xocl_md_del_endpoint(dev, blob, ep->ep_name, NULL);

	return ret;
}

int xocl_md_add_endpoint(struct device *dev, char **blob,
	struct xocl_md_endpoint *ep)
{
	return __xocl_md_add_endpoint(dev, blob, ep, NULL);
}

static int xocl_md_get_endpoint(struct device *dev, char *blob,
	const char *ep_name, char *regmap_name, int *ep_offset)
{
	int offset;
	const char *name;

	for (offset = fdt_next_node(blob, -1, NULL);
	    offset >= 0;
	    offset = fdt_next_node(blob, offset, NULL)) {
		name = fdt_get_name(blob, offset, NULL);
		if (!name || strncmp(name, ep_name, strlen(ep_name)))
			continue;
		if (!regmap_name ||
		    !fdt_node_check_compatible(blob, offset, regmap_name))
			break;
	}
	if (offset < 0)
		return -ENODEV;

	*ep_offset = offset;

	return 0;
}

int xocl_md_get_prop(struct device *dev, char *blob, const char *ep_name,
	char *regmap_name, char *prop, const void **val, int *size)
{
	int offset;
	int ret;

	*val = NULL;
	if (ep_name) {
		ret = xocl_md_get_endpoint(dev, blob, ep_name, regmap_name,
			&offset);
		if (ret) {
			md_err(dev, "cannot get ep %s, regmap %s, ret = %d",
				ep_name, regmap_name, ret);
			return -EINVAL;
		}
	} else {
		offset = fdt_next_node(blob, -1, NULL);
		if (offset < 0) {
			md_err(dev, "internal error, ret = %d", offset);
			return -EINVAL;
		}
	}

	*val = fdt_getprop(blob, offset, prop, size);
	if (!*val) {
		md_dbg(dev, "get prop failed, ep %s, prop %s", ep_name, prop);
		return -EINVAL;
	}

	return 0;
}

static int xocl_md_setprop(struct device *dev, char **blob, int offset,
	 const char *prop, const void *val, int size)
{
	int ret;
	char *buf = NULL;
	int bufsz;

	ret = fdt_setprop(*blob, offset, prop, val, size);
	if (ret == -FDT_ERR_NOSPACE) {
		bufsz = fdt_totalsize(*blob) + size + BLOB_DELTA;
		buf = vmalloc(bufsz);
		if (!buf)
			return -ENOMEM;
		ret = fdt_resize(*blob, buf, bufsz);
		if (ret) {
			md_err(dev, "resize failed, ret = %d", ret);
			goto failed;
		}
		vfree(*blob);
		*blob = buf;
		buf = NULL;

		ret = fdt_setprop(*blob, offset, prop, val, size);
		if (ret) {
			md_err(dev, "set prop failed, ret = %d", ret);
			goto failed;
		}
	}

failed:
	if (ret && buf)
		vfree(buf);

	return ret;
}

int xocl_md_set_prop(struct device *dev, char **blob,
	const char *ep_name, char *regmap_name, char *prop, void *val, int size)
{
	int offset;
	int ret;

	if (ep_name) {
		ret = xocl_md_get_endpoint(dev, *blob, ep_name,
			regmap_name, &offset);
		if (ret) {
			md_err(dev, "cannot get node %s, ret = %d",
				ep_name, ret);
			return -EINVAL;
		}
	} else {
		offset = fdt_next_node(blob, -1, NULL);
		if (offset < 0) {
			md_err(dev, "internal error, ret = %d", offset);
			return -EINVAL;
		}
	}

	ret = xocl_md_setprop(dev, blob, offset, prop, val, size);
	if (ret)
		md_err(dev, "set prop failed, ret = %d", ret);

	return ret;
}

int xocl_md_copy_endpoint(struct device *dev, char **blob, char *src_blob,
	const char *ep_name, char *regmap_name)
{
	int offset, target;
	int ret;
	struct xocl_md_endpoint ep = {0};

	ret = xocl_md_get_endpoint(dev, src_blob, ep_name, regmap_name,
		&offset);
	if (ret)
		return -EINVAL;

	ret = xocl_md_get_endpoint(dev, *blob, ep_name, regmap_name, &target);
	if (ret) {
		ep.ep_name = ep_name;
		ret = __xocl_md_add_endpoint(dev, blob, &ep, &target);
		if (ret)
			return -EINVAL;
	}

	ret = xocl_md_overlay(dev, blob, target, src_blob, offset);
	if (ret)
		md_err(dev, "overlay failed, ret = %d", ret);

	return ret;
}

int xocl_md_copy_all_eps(struct device *dev, char **blob, char *src_blob)
{
	return xocl_md_overlay(dev, blob, -1, src_blob, -1);
}

static int xocl_md_overlay(struct device *dev, char **blob, int target,
	char *overlay_blob, int overlay_offset)
{
	int	property, subnode;
	int	ret;

	WARN_ON(!blob || !overlay_blob);

	if (!*blob) {
		md_err(dev, "blob is NULL");
		return -EINVAL;
	}

	if (target < 0) {
		xocl_md_get_endpoint(dev, *blob, NODE_ENDPOINTS, NULL,
			&target);
	}
	if (overlay_offset < 0) {
		xocl_md_get_endpoint(dev, overlay_blob, NODE_ENDPOINTS, NULL,
			&overlay_offset);
	}

	fdt_for_each_property_offset(property, overlay_blob, overlay_offset) {
		const char *name;
		const void *prop;
		int prop_len;

		prop = fdt_getprop_by_offset(overlay_blob, property, &name,
			&prop_len);
		if (prop < 0) {
			md_err(dev, "internal error");
			return -EINVAL;
		}

		ret = xocl_md_setprop(dev, blob, target, name, prop,
			prop_len);
		if (ret) {
			md_err(dev, "setprop failed, ret = %d", ret);
			return ret;
		}
	}

	fdt_for_each_subnode(subnode, overlay_blob, overlay_offset) {
		const char *name = fdt_get_name(overlay_blob, subnode, NULL);
		int nnode;

		nnode = xocl_md_add_node(dev, blob, target, name);
		if (nnode == -FDT_ERR_EXISTS)
			nnode = fdt_subnode_offset(*blob, target, name);
		if (nnode < 0) {
			md_err(dev, "add node failed, ret = %d", nnode);
			return nnode;
		}

		ret = xocl_md_overlay(dev, blob, nnode, overlay_blob, subnode);
		if (ret)
			return ret;
	}

	return 0;
}

int xocl_md_get_next_endpoint(struct device *dev, char *blob,
	const char *ep_name, char *regmap_name,
	char **next_ep, char **next_regmap)
{
	int offset, ret;

	if (!ep_name) {
		ret = xocl_md_get_endpoint(dev, blob, NODE_ENDPOINTS, NULL,
			&offset);
	} else {
		ret = xocl_md_get_endpoint(dev, blob, ep_name, regmap_name,
			&offset);
	}

	if (ret) {
		*next_ep = NULL;
		*next_regmap = NULL;
		return -EINVAL;
	}

	offset = ep_name ? fdt_next_subnode(blob, offset) :
		fdt_first_subnode(blob, offset);
	if (offset < 0) {
		*next_ep = NULL;
		*next_regmap = NULL;
		return -EINVAL;
	}

	*next_ep = (char *)fdt_get_name(blob, offset, NULL);
	*next_regmap = (char *)fdt_stringlist_get(blob, offset, PROP_COMPATIBLE,
		0, NULL);

	return 0;
}

int xocl_md_get_compatible_epname(struct device *dev, char *blob,
	const char *regmap_name, char **ep_name)
{
	int ep_offset;

	ep_offset = fdt_node_offset_by_compatible(blob, -1, regmap_name);
	if (ep_offset < 0) {
		*ep_name = NULL;
		return -ENOENT;
	}

	*ep_name = (char *)fdt_get_name(blob, ep_offset, NULL);

	return 0;
}
