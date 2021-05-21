// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA PCIe Firewall Driver
 *
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou<Lizhi.Hou@xilinx.com>
 */

/*
 * the PCIe Firewall IP to protect against host access to BARs which are not
 * available i.e. when the PLP(Provider Logic Partition) is in reset, not yet
 * configured or not implemented.
 *
 * Following server warm/cold boot or hot reset, the PCIe Firewall will
 * automatically respond to accesses to BARs implemented in the PLP for compute
 * platforms i.e.
 *    PF0, BAR2
 *    PF1, BAR2
 *    PF1, BAR4
 * Once the PLP has been programmed and ep_pr_isolate_plp_00 has been released
 * from reset, then XRT should program the PCIe Firewall IP to clear the
 * appropriate bits in the Enable Response Register (0x8) to allow transactions
 * to propagate to the PLP.
 */

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
#include "xleaf/pcie-firewall.h"

#define XRT_PCIE_FIREWALL	"xrt_pcie_firewall"
#define XRT_PFW_REG_EN_RESP	8
#define XRT_PFW_UNBLOCK_BIT(pf, bar)	(1U << ((pf) * 6 + (bar)))

XRT_DEFINE_REGMAP_CONFIG(pfw_regmap_config);

struct xrt_pfw {
	struct xrt_device	*xdev;
	struct regmap		*regmap;
	struct mutex		pfw_lock; /* register access lock */
};

static int xrt_pfw_unblock(struct xrt_pfw *pfw, struct xrt_pcie_firewall_unblock *arg)
{
	u32 val;
	int ret;

	mutex_lock(&pfw->pfw_lock);
	ret = regmap_read(pfw->regmap, XRT_PFW_REG_EN_RESP, &val);
	if (ret) {
		xrt_err(pfw->xdev, "read en_resp register failed");
		goto failed;
	}
	if (val & XRT_PFW_UNBLOCK_BIT(arg->pf_index, arg->bar_index)) {
		xrt_info(pfw->xdev, "unblock pf%d, bar%d", arg->pf_index, arg->bar_index);
		val = (~XRT_PFW_UNBLOCK_BIT(arg->pf_index, arg->bar_index)) & val;
		ret = regmap_write(pfw->regmap, XRT_PFW_REG_EN_RESP, val);
		if (ret) {
			xrt_err(pfw->xdev, "write en_resp register failed");
			goto failed;
		}
	}
	mutex_unlock(&pfw->pfw_lock);

	return 0;

failed:
	return ret;
}

static int xrt_pfw_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct xrt_pfw *pfw;
	int ret = 0;

	pfw = xrt_get_drvdata(xdev);

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		/* Does not handle any event. */
		break;
	case XRT_PFW_UNBLOCK:
		ret = xrt_pfw_unblock(pfw, arg);
		break;
	default:
		xrt_err(xdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xrt_pfw_probe(struct xrt_device *xdev)
{
	struct xrt_pfw *pfw = NULL;
	void __iomem *base = NULL;
	struct resource *res;
	int ret;

	pfw = devm_kzalloc(&xdev->dev, sizeof(*pfw), GFP_KERNEL);
	if (!pfw)
		return -ENOMEM;

	xrt_set_drvdata(xdev, pfw);
	pfw->xdev = xdev;
	mutex_init(&pfw->pfw_lock);

	res = xrt_get_resource(xdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -EINVAL;
		goto failed;
	}
	base = devm_ioremap_resource(&xdev->dev, res);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto failed;
	}

	pfw->regmap = devm_regmap_init_mmio(&xdev->dev, base, &pfw_regmap_config);
	if (IS_ERR(pfw->regmap)) {
		xrt_err(xdev, "regmap %pR failed", res);
		ret = PTR_ERR(pfw->regmap);
		goto failed;
	}
	xrt_info(xdev, "successfully initialized");

	return 0;

failed:
	return ret;
}

static struct xrt_dev_endpoints xrt_pfw_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names[]) {
			{ .ep_name = XRT_MD_NODE_PCIE_FIREWALL },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

static struct xrt_driver xrt_pfw_driver = {
	.driver = {
		.name = XRT_PCIE_FIREWALL,
	},
	.subdev_id = XRT_SUBDEV_PCIE_FIREWALL,
	.endpoints = xrt_pfw_endpoints,
	.probe = xrt_pfw_probe,
	.leaf_call = xrt_pfw_leaf_call,
};

XRT_LEAF_INIT_FINI_FUNC(pfw);
