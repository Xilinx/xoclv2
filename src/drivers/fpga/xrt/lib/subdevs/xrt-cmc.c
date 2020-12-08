// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA CMC Leaf Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include "xrt-metadata.h"
#include "xrt-subdev.h"
#include "xrt-cmc-impl.h"
#include "xrt-cmc.h"
#include <linux/xrt/mailbox_proto.h>

#define	XRT_CMC "xrt_cmc"

static struct xrt_iores_map cmc_iores_id_map[] = {
	{ NODE_CMC_REG, IO_REG},
	{ NODE_CMC_RESET, IO_GPIO},
	{ NODE_CMC_FW_MEM, IO_IMAGE_MGMT},
	{ NODE_CMC_MUTEX, IO_MUTEX},
};

struct xrt_cmc {
	struct platform_device *pdev;
	struct cmc_reg_map regs[NUM_IOADDR];
	void *ctrl_hdl;
	void *sensor_hdl;
	void *mbx_hdl;
	void *bdinfo_hdl;
	void *sc_hdl;
};

void *cmc_pdev2sc(struct platform_device *pdev)
{
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);

	return cmc->sc_hdl;
}

void *cmc_pdev2bdinfo(struct platform_device *pdev)
{
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);

	return cmc->bdinfo_hdl;
}

void *cmc_pdev2ctrl(struct platform_device *pdev)
{
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);

	return cmc->ctrl_hdl;
}

void *cmc_pdev2sensor(struct platform_device *pdev)
{
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);

	return cmc->sensor_hdl;
}

void *cmc_pdev2mbx(struct platform_device *pdev)
{
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);

	return cmc->mbx_hdl;
}

static int cmc_map_io(struct xrt_cmc *cmc, struct resource *res)
{
	int	id;

	id = xrt_md_res_name2id(cmc_iores_id_map, ARRAY_SIZE(cmc_iores_id_map),
		res->name);
	if (id < 0) {
		xrt_err(cmc->pdev, "resource %s ignored", res->name);
		return -EINVAL;
	}
	if (cmc->regs[id].crm_addr) {
		xrt_err(cmc->pdev, "resource %s already mapped", res->name);
		return -EINVAL;
	}
	cmc->regs[id].crm_addr = ioremap(res->start, res->end - res->start + 1);
	if (!cmc->regs[id].crm_addr) {
		xrt_err(cmc->pdev, "resource %s map failed", res->name);
		return -EIO;
	}
	cmc->regs[id].crm_size = res->end - res->start + 1;

	return 0;
}

static int cmc_remove(struct platform_device *pdev)
{
	int i;
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);

	xrt_info(pdev, "leaving...");

	cmc_sc_remove(pdev);
	cmc_bdinfo_remove(pdev);
	cmc_mailbox_remove(pdev);
	cmc_sensor_remove(pdev);
	cmc_ctrl_remove(pdev);

	for (i = 0; i < NUM_IOADDR; i++) {
		if (cmc->regs[i].crm_addr == NULL)
			continue;
		iounmap(cmc->regs[i].crm_addr);
	}

	return 0;
}

static int cmc_probe(struct platform_device *pdev)
{
	struct xrt_cmc *cmc;
	struct resource *res;
	int i = 0;
	int ret = 0;

	xrt_info(pdev, "probing...");

	cmc = devm_kzalloc(DEV(pdev), sizeof(*cmc), GFP_KERNEL);
	if (!cmc)
		return -ENOMEM;

	cmc->pdev = pdev;
	platform_set_drvdata(pdev, cmc);

	for (i = 0; ; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			break;
		(void) cmc_map_io(cmc, res);
	}
	for (i = 0; i < NUM_IOADDR; i++) {
		if (cmc->regs[i].crm_addr == NULL)
			break;
	}
	if (i != NUM_IOADDR) {
		xrt_err(cmc->pdev, "not all needed resources are found");
		ret = -EINVAL;
		goto done;
	}

	ret = cmc_ctrl_probe(cmc->pdev, cmc->regs, &cmc->ctrl_hdl);
	if (ret)
		goto done;

	/* Non-critical part of init can fail. */
	(void) cmc_sensor_probe(cmc->pdev, cmc->regs, &cmc->sensor_hdl);
	(void) cmc_mailbox_probe(cmc->pdev, cmc->regs, &cmc->mbx_hdl);
	(void) cmc_bdinfo_probe(cmc->pdev, cmc->regs, &cmc->bdinfo_hdl);
	(void) cmc_sc_probe(cmc->pdev, cmc->regs, &cmc->sc_hdl);

	return 0;

done:
	(void) cmc_remove(pdev);
	return ret;
}

static int
xrt_cmc_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xrt_cmc *cmc = platform_get_drvdata(pdev);
	int ret = -ENOENT;

	switch (cmd) {
	case XRT_CMC_READ_BOARD_INFO: {
		struct xcl_board_info *i = (struct xcl_board_info *)arg;

		if (cmc->bdinfo_hdl)
			ret = cmc_bdinfo_read(pdev, i);
		break;
	}
	case XRT_CMC_READ_SENSORS: {
		struct xcl_sensor *s = (struct xcl_sensor *)arg;

		if (cmc->sensor_hdl) {
			cmc_sensor_read(pdev, s);
			ret = 0;
		}
		break;
	}
	default:
		xrt_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

struct xrt_subdev_endpoints xrt_cmc_endpoints[] = {
	{
		.xse_names = (struct xrt_subdev_ep_names []) {
			{ .ep_name = NODE_CMC_REG },
			{ .ep_name = NODE_CMC_RESET },
			{ .ep_name = NODE_CMC_MUTEX },
			{ .ep_name = NODE_CMC_FW_MEM },
			{ NULL },
		},
		.xse_min_ep = 4,
	},
	{ 0 },
};

struct xrt_subdev_drvdata xrt_cmc_data = {
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = cmc_sc_open,
			.release = cmc_sc_close,
			.llseek = cmc_sc_llseek,
			.write = cmc_update_sc_firmware,
		},
		.xsf_dev_name = "cmc",
	},
	.xsd_dev_ops = {
		.xsd_ioctl = xrt_cmc_leaf_ioctl,
	},
};

static const struct platform_device_id cmc_id_table[] = {
	{ XRT_CMC, (kernel_ulong_t)&xrt_cmc_data },
	{ },
};

struct platform_driver xrt_cmc_driver = {
	.driver	= {
		.name    = XRT_CMC,
	},
	.probe   = cmc_probe,
	.remove  = cmc_remove,
	.id_table = cmc_id_table,
};
