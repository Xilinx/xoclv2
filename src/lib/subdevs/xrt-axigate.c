// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA AXI Gate Driver
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
#include "xocl-axigate.h"

#define XOCL_AXIGATE "xocl_axigate"

struct axigate_regs {
	u32		iag_wr;
	u32		iag_rvsd;
	u32		iag_rd;
} __packed;

struct xocl_axigate {
	struct platform_device	*pdev;
	void			*base;
	struct mutex		gate_lock;

	void			*evt_hdl;
	const char		*ep_name;

	bool			gate_freezed;
};

#define reg_rd(g, r)						\
	ioread32(&((struct axigate_regs *)g->base)->r)
#define reg_wr(g, v, r)						\
	iowrite32(v, &((struct axigate_regs *)g->base)->r)

#define freeze_gate(gate)			\
	do {					\
		reg_wr(gate, 0, iag_wr);	\
		ndelay(500);			\
		reg_rd(gate, iag_rd);		\
	} while (0)

#define free_gate(gate)				\
	do {					\
		reg_wr(gate, 0x2, iag_wr);	\
		ndelay(500);			\
		(void) reg_rd(gate, iag_rd);	\
		reg_wr(gate, 0x3, iag_wr);	\
		ndelay(500);			\
		reg_rd(gate, iag_rd);		\
	} while (0)				\

static int xocl_axigate_epname_idx(struct platform_device *pdev)
{
	int			i;
	int			ret;
	struct resource		*res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xocl_err(pdev, "Empty Resource!");
		return -EINVAL;
	}

	for (i = 0; xocl_axigate_epnames[i]; i++) {
		ret = strncmp(xocl_axigate_epnames[i], res->name,
			strlen(xocl_axigate_epnames[i]) + 1);
		if (!ret)
			break;
	}

	return (xocl_axigate_epnames[i]) ? i : -EINVAL;
}

static bool xocl_axigate_leaf_match(enum xocl_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	const char		*ep_name = arg;
	struct resource		*res;

	if (id != XOCL_SUBDEV_AXIGATE)
		return false;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xocl_err(pdev, "Empty Resource!");
		return false;
	}

	if (strncmp(res->name, ep_name, strlen(res->name) + 1))
		return true;

	return false;
}

static void xocl_axigate_freeze(struct platform_device *pdev)
{
	struct xocl_axigate	*gate;
	u32			freeze = 0;

	gate = platform_get_drvdata(pdev);

	mutex_lock(&gate->gate_lock);
	freeze = reg_rd(gate, iag_rd);
	if (freeze) {		/* gate is opened */
		xocl_subdev_broadcast_event(pdev, XOCL_EVENT_PRE_GATE_CLOSE);
		freeze_gate(gate);
	}

	gate->gate_freezed = true;
	mutex_unlock(&gate->gate_lock);

	xocl_info(pdev, "freeze gate %s", gate->ep_name);
}

static void xocl_axigate_free(struct platform_device *pdev)
{
	struct xocl_axigate	*gate;
	u32			freeze;

	gate = platform_get_drvdata(pdev);

	mutex_lock(&gate->gate_lock);
	freeze = reg_rd(gate, iag_rd);
	if (!freeze) {		/* gate is closed */
		free_gate(gate);
		xocl_subdev_broadcast_event_async(pdev,
			XOCL_EVENT_POST_GATE_OPEN, NULL, NULL);
		/* xocl_axigate_free() could be called in event cb, thus
		 * we can not wait for the completes
		 */
	}

	gate->gate_freezed = false;
	mutex_unlock(&gate->gate_lock);

	xocl_info(pdev, "free gate %s", gate->ep_name);
}

static int
xocl_axigate_event_cb(struct platform_device *pdev,
	enum xocl_events evt, void *arg)
{
	struct platform_device *leaf;
	struct xocl_event_arg_subdev *esd = (struct xocl_event_arg_subdev *)arg;
	enum xocl_subdev_id id;
	int instance;

	switch (evt) {
	case XOCL_EVENT_POST_CREATION:
		break;
	default:
		return XOCL_EVENT_CB_CONTINUE;
	}

	id = esd->xevt_subdev_id;
	instance = esd->xevt_subdev_instance;

	/*
	 * higher level axigate instance created,
	 * make sure the gate is openned. This covers 1RP flow which
	 * has plp gate as well.
	 */
	leaf = xocl_subdev_get_leaf_by_id(pdev, id, instance);
	if (leaf) {
		if (xocl_axigate_epname_idx(leaf) >
		    xocl_axigate_epname_idx(pdev))
			xocl_axigate_free(pdev);
		else
			xocl_subdev_ioctl(leaf, XOCL_AXIGATE_FREE, NULL);
		xocl_subdev_put_leaf(pdev, leaf);
	}

	return XOCL_EVENT_CB_CONTINUE;
}

static int
xocl_axigate_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	switch (cmd) {
	case XOCL_AXIGATE_FREEZE:
		xocl_axigate_freeze(pdev);
		break;
	case XOCL_AXIGATE_FREE:
		xocl_axigate_free(pdev);
		break;
	default:
		xocl_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return 0;
}

static int xocl_axigate_remove(struct platform_device *pdev)
{
	struct xocl_axigate	*gate;

	gate = platform_get_drvdata(pdev);

	if (gate->base)
		iounmap(gate->base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, gate);

	return 0;
}

static int xocl_axigate_probe(struct platform_device *pdev)
{
	struct xocl_axigate	*gate;
	struct resource		*res;
	int			ret;

	gate = devm_kzalloc(&pdev->dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return -ENOMEM;

	gate->pdev = pdev;
	platform_set_drvdata(pdev, gate);

	xocl_info(pdev, "probing...");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xocl_err(pdev, "Empty resource 0");
		ret = -EINVAL;
		goto failed;
	}

	gate->base = ioremap(res->start, res->end - res->start + 1);
	if (!gate->base) {
		xocl_err(pdev, "map base iomem failed");
		ret = -EFAULT;
		goto failed;
	}

	gate->evt_hdl = xocl_subdev_add_event_cb(pdev,
		xocl_axigate_leaf_match, (void *)res->name,
		xocl_axigate_event_cb);

	gate->ep_name = res->name;

	mutex_init(&gate->gate_lock);

	return 0;

failed:
	xocl_axigate_remove(pdev);
	return ret;
}

struct xocl_subdev_endpoints xocl_axigate_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = "ep_pr_isolate_ulp_00" },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = "ep_pr_isolate_plp_00" },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xocl_subdev_drvdata xocl_axigate_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_axigate_leaf_ioctl,
	},
};

static const struct platform_device_id xocl_axigate_table[] = {
	{ XOCL_AXIGATE, (kernel_ulong_t)&xocl_axigate_data },
	{ },
};

struct platform_driver xocl_axigate_driver = {
	.driver = {
		.name = XOCL_AXIGATE,
	},
	.probe = xocl_axigate_probe,
	.remove = xocl_axigate_remove,
	.id_table = xocl_axigate_table,
};
