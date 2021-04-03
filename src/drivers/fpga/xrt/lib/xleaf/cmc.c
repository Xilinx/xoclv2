// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA CMC Leaf Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/io.h>
#include "metadata.h"
#include "xleaf.h"
#include "xrt-cmc-impl.h"
#include "xleaf/cmc.h"
#include <linux/xrt/mailbox_proto.h>

#define XRT_CMC "xrt_cmc"

static struct xrt_iores_map cmc_iores_id_map[] = {
	{ XRT_MD_NODE_CMC_REG, IO_REG},
	{ XRT_MD_NODE_CMC_RESET, IO_GPIO},
	{ XRT_MD_NODE_CMC_FW_MEM, IO_IMAGE_MGMT},
	{ XRT_MD_NODE_CMC_MUTEX, IO_MUTEX},
};

struct xrt_cmc {
	struct xrt_device *xdev;
	struct cmc_reg_map regs[NUM_IOADDR];
	void *ctrl_hdl;
	void *sensor_hdl;
	void *mbx_hdl;
	void *bdinfo_hdl;
	void *sc_hdl;
};

void *cmc_xdev2sc(struct xrt_device *xdev)
{
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);

	return cmc->sc_hdl;
}

void *cmc_xdev2bdinfo(struct xrt_device *xdev)
{
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);

	return cmc->bdinfo_hdl;
}

void *cmc_xdev2ctrl(struct xrt_device *xdev)
{
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);

	return cmc->ctrl_hdl;
}

void *cmc_xdev2sensor(struct xrt_device *xdev)
{
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);

	return cmc->sensor_hdl;
}

void *cmc_xdev2mbx(struct xrt_device *xdev)
{
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);

	return cmc->mbx_hdl;
}

static int cmc_map_io(struct xrt_cmc *cmc, struct resource *res)
{
	int	id;

	id = xrt_md_res_name2id(cmc_iores_id_map, ARRAY_SIZE(cmc_iores_id_map), res->name);
	if (id < 0) {
		xrt_err(cmc->xdev, "resource %s ignored", res->name);
		return -EINVAL;
	}
	if (cmc->regs[id].crm_addr) {
		xrt_err(cmc->xdev, "resource %s already mapped", res->name);
		return -EINVAL;
	}
	cmc->regs[id].crm_addr = ioremap(res->start, res->end - res->start + 1);
	if (!cmc->regs[id].crm_addr) {
		xrt_err(cmc->xdev, "resource %s map failed", res->name);
		return -EIO;
	}
	cmc->regs[id].crm_size = res->end - res->start + 1;

	return 0;
}

static void cmc_remove(struct xrt_device *xdev)
{
	int i;
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);

	xrt_info(xdev, "leaving...");

	cmc_sc_remove(xdev);
	cmc_bdinfo_remove(xdev);
	cmc_mailbox_remove(xdev);
	cmc_sensor_remove(xdev);
	cmc_ctrl_remove(xdev);

	for (i = 0; i < NUM_IOADDR; i++) {
		if (!cmc->regs[i].crm_addr)
			continue;
		iounmap(cmc->regs[i].crm_addr);
	}
}

static int cmc_probe(struct xrt_device *xdev)
{
	struct xrt_cmc *cmc;
	struct resource *res;
	int i = 0;
	int ret = 0;

	xrt_info(xdev, "probing...");

	cmc = devm_kzalloc(DEV(xdev), sizeof(*cmc), GFP_KERNEL);
	if (!cmc)
		return -ENOMEM;

	cmc->xdev = xdev;
	xrt_set_drvdata(xdev, cmc);

	for (i = 0; ; i++) {
		res = xrt_get_resource(xdev, IORESOURCE_MEM, i);
		if (!res)
			break;
		cmc_map_io(cmc, res);
	}
	for (i = 0; i < NUM_IOADDR; i++) {
		if (!cmc->regs[i].crm_addr)
			break;
	}
	if (i != NUM_IOADDR) {
		xrt_err(cmc->xdev, "not all needed resources are found");
		ret = -EINVAL;
		goto done;
	}

	ret = cmc_ctrl_probe(cmc->xdev, cmc->regs, &cmc->ctrl_hdl);
	if (ret)
		goto done;

	/* Non-critical part of init can fail. */
	cmc_sensor_probe(cmc->xdev, cmc->regs, &cmc->sensor_hdl);
	cmc_mailbox_probe(cmc->xdev, cmc->regs, &cmc->mbx_hdl);
	cmc_bdinfo_probe(cmc->xdev, cmc->regs, &cmc->bdinfo_hdl);
	cmc_sc_probe(cmc->xdev, cmc->regs, &cmc->sc_hdl);

	return 0;

done:
	cmc_remove(xdev);
	return ret;
}

static int
xrt_cmc_leaf_call(struct xrt_device *xdev, u32 cmd, void *arg)
{
	struct xrt_cmc *cmc = xrt_get_drvdata(xdev);
	int ret = -ENOENT;

	switch (cmd) {
	case XRT_XLEAF_EVENT:
		cmc_ctrl_event_cb(xdev, arg);
		break;
	case XRT_CMC_READ_BOARD_INFO: {
		struct xcl_board_info *i = (struct xcl_board_info *)arg;

		if (cmc->bdinfo_hdl)
			ret = cmc_bdinfo_read(xdev, i);
		break;
	}
	case XRT_CMC_READ_SENSORS: {
		struct xcl_sensor *s = (struct xcl_sensor *)arg;

		if (cmc->sensor_hdl) {
			cmc_sensor_read(xdev, s);
			ret = 0;
		}
		break;
	}
	default:
		xrt_err(xdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static struct xrt_dev_endpoints xrt_cmc_endpoints[] = {
	{
		.xse_names = (struct xrt_dev_ep_names []) {
			{ .ep_name = XRT_MD_NODE_CMC_REG },
			{ .ep_name = XRT_MD_NODE_CMC_RESET },
			{ .ep_name = XRT_MD_NODE_CMC_MUTEX },
			{ .ep_name = XRT_MD_NODE_CMC_FW_MEM },
			{ NULL },
		},
		.xse_min_ep = 4,
	},
	{ 0 },
};

static struct xrt_driver xrt_cmc_driver = {
	.driver	= {
		.name = XRT_CMC,
	},
	.file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = cmc_sc_open,
			.release = cmc_sc_close,
			.llseek = cmc_sc_llseek,
			.write = cmc_update_sc_firmware,
		},
		.xsf_dev_name = "cmc",
	},
	.subdev_id = XRT_SUBDEV_CMC,
	.endpoints = xrt_cmc_endpoints,
	.probe = cmc_probe,
	.remove = cmc_remove,
	.leaf_call = xrt_cmc_leaf_call,
};

XRT_LEAF_INIT_FINI_FUNC(cmc);
