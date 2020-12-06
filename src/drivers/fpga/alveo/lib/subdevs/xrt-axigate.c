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
#include "xrt-metadata.h"
#include "xrt-subdev.h"
#include "xrt-parent.h"
#include "xrt-axigate.h"

#define XRT_AXIGATE "xrt_axigate"

struct axigate_regs {
	u32		iag_wr;
	u32		iag_rvsd;
	u32		iag_rd;
} __packed;

struct xrt_axigate {
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

static int xrt_axigate_epname_idx(struct platform_device *pdev)
{
	int			i;
	int			ret;
	struct resource		*res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xrt_err(pdev, "Empty Resource!");
		return -EINVAL;
	}

	for (i = 0; xrt_axigate_epnames[i]; i++) {
		ret = strncmp(xrt_axigate_epnames[i], res->name,
			strlen(xrt_axigate_epnames[i]) + 1);
		if (!ret)
			break;
	}

	return (xrt_axigate_epnames[i]) ? i : -EINVAL;
}

static bool xrt_axigate_leaf_match(enum xrt_subdev_id id,
	struct platform_device *pdev, void *arg)
{
	const char		*ep_name = arg;
	struct resource		*res;

	if (id != XRT_SUBDEV_AXIGATE)
		return false;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xrt_err(pdev, "Empty Resource!");
		return false;
	}

	if (strncmp(res->name, ep_name, strlen(res->name) + 1))
		return true;

	return false;
}

static void xrt_axigate_freeze(struct platform_device *pdev)
{
	struct xrt_axigate	*gate;
	u32			freeze = 0;

	gate = platform_get_drvdata(pdev);

	mutex_lock(&gate->gate_lock);
	freeze = reg_rd(gate, iag_rd);
	if (freeze) {		/* gate is opened */
		xrt_subdev_broadcast_event(pdev, XRT_EVENT_PRE_GATE_CLOSE);
		freeze_gate(gate);
	}

	gate->gate_freezed = true;
	mutex_unlock(&gate->gate_lock);

	xrt_info(pdev, "freeze gate %s", gate->ep_name);
}

static void xrt_axigate_free(struct platform_device *pdev)
{
	struct xrt_axigate	*gate;
	u32			freeze;

	gate = platform_get_drvdata(pdev);

	mutex_lock(&gate->gate_lock);
	freeze = reg_rd(gate, iag_rd);
	if (!freeze) {		/* gate is closed */
		free_gate(gate);
		xrt_subdev_broadcast_event_async(pdev,
			XRT_EVENT_POST_GATE_OPEN, NULL, NULL);
		/* xrt_axigate_free() could be called in event cb, thus
		 * we can not wait for the completes
		 */
	}

	gate->gate_freezed = false;
	mutex_unlock(&gate->gate_lock);

	xrt_info(pdev, "free gate %s", gate->ep_name);
}

static int
xrt_axigate_event_cb(struct platform_device *pdev,
	enum xrt_events evt, void *arg)
{
	struct platform_device *leaf;
	struct xrt_event_arg_subdev *esd = (struct xrt_event_arg_subdev *)arg;
	enum xrt_subdev_id id;
	int instance;

	switch (evt) {
	case XRT_EVENT_POST_CREATION:
		break;
	default:
		return XRT_EVENT_CB_CONTINUE;
	}

	id = esd->xevt_subdev_id;
	instance = esd->xevt_subdev_instance;

	/*
	 * higher level axigate instance created,
	 * make sure the gate is openned. This covers 1RP flow which
	 * has plp gate as well.
	 */
	leaf = xrt_subdev_get_leaf_by_id(pdev, id, instance);
	if (leaf) {
		if (xrt_axigate_epname_idx(leaf) >
		    xrt_axigate_epname_idx(pdev))
			xrt_axigate_free(pdev);
		else
			xrt_subdev_ioctl(leaf, XRT_AXIGATE_FREE, NULL);
		xrt_subdev_put_leaf(pdev, leaf);
	}

	return XRT_EVENT_CB_CONTINUE;
}

static int
xrt_axigate_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	switch (cmd) {
	case XRT_AXIGATE_FREEZE:
		xrt_axigate_freeze(pdev);
		break;
	case XRT_AXIGATE_FREE:
		xrt_axigate_free(pdev);
		break;
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return 0;
}

static int xrt_axigate_remove(struct platform_device *pdev)
{
	struct xrt_axigate	*gate;

	gate = platform_get_drvdata(pdev);

	if (gate->base)
		iounmap(gate->base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, gate);

	return 0;
}

static int xrt_axigate_probe(struct platform_device *pdev)
{
	struct xrt_axigate	*gate;
	struct resource		*res;
	int			ret;

	gate = devm_kzalloc(&pdev->dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return -ENOMEM;

	gate->pdev = pdev;
	platform_set_drvdata(pdev, gate);

	xrt_info(pdev, "probing...");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xrt_err(pdev, "Empty resource 0");
		ret = -EINVAL;
		goto failed;
	}

	gate->base = ioremap(res->start, res->end - res->start + 1);
	if (!gate->base) {
		xrt_err(pdev, "map base iomem failed");
		ret = -EFAULT;
		goto failed;
	}

	gate->evt_hdl = xrt_subdev_add_event_cb(pdev,
		xrt_axigate_leaf_match, (void *)res->name,
		xrt_axigate_event_cb);

	gate->ep_name = res->name;

	mutex_init(&gate->gate_lock);

	return 0;

failed:
	xrt_axigate_remove(pdev);
	return ret;
}

struct xrt_subdev_endpoints xrt_axigate_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = "ep_pr_isolate_ulp_00" },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{
		.xse_names = (struct xrt_subdev_ep_names[]) {
			{ .ep_name = "ep_pr_isolate_plp_00" },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xrt_subdev_drvdata xrt_axigate_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_axigate_leaf_ioctl,
	},
};

static const struct platform_device_id xrt_axigate_table[] = {
	{ XRT_AXIGATE, (kernel_ulong_t)&xrt_axigate_data },
	{ },
};

struct platform_driver xrt_axigate_driver = {
	.driver = {
		.name = XRT_AXIGATE,
	},
	.probe = xrt_axigate_probe,
	.remove = xrt_axigate_remove,
	.id_table = xrt_axigate_table,
};
