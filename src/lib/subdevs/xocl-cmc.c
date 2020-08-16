// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA CMC Leaf Driver
 *
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include "xocl-metadata.h"
#include "xocl-subdev.h"
#include "xocl-cmc-impl.h"

#define	XOCL_CMC "xocl_cmc"

static struct xocl_iores_map cmc_iores_id_map[] = {
	{ NODE_CMC_REG, IO_REG},
	{ NODE_CMC_RESET, IO_GPIO},
	{ NODE_CMC_FW_MEM, IO_IMAGE_MGMT},
	{ NODE_CMC_MUTEX, IO_MUTEX},
};

struct xocl_cmc {
	struct platform_device *pdev;
	struct cmc_reg_map regs[NUM_IOADDR];
	void *ctrl_hdl;
};

static int cmc_map_io(struct xocl_cmc *cmc, struct resource *res)
{
	int	id;

	id = xocl_md_res_name2id(cmc_iores_id_map, ARRAY_SIZE(cmc_iores_id_map),
		res->name);
	if (id < 0) {
		xocl_err(cmc->pdev, "resource %s ignored", res->name);
		return -EINVAL;
	}
	if (cmc->regs[id].crm_addr) {
		xocl_err(cmc->pdev, "resource %s already mapped", res->name);
		return -EINVAL;
	}
	cmc->regs[id].crm_addr = ioremap(res->start, res->end - res->start + 1);
	if (!cmc->regs[id].crm_addr) {
		xocl_err(cmc->pdev, "resource %s map failed", res->name);
		return -EIO;
	}
	cmc->regs[id].crm_size = res->end - res->start + 1;

	return 0;
}

static int cmc_remove(struct platform_device *pdev)
{
	int i;
	struct xocl_cmc *cmc = platform_get_drvdata(pdev);

	xocl_info(pdev, "leaving...");

	if (cmc->ctrl_hdl)
		cmc_ctrl_remove(cmc->ctrl_hdl);

	for (i = 0; i < NUM_IOADDR; i++) {
		if (cmc->regs[i].crm_addr == NULL)
			continue;
		iounmap(cmc->regs[i].crm_addr);
	}

	return 0;
}

static int cmc_probe(struct platform_device *pdev)
{
	struct xocl_cmc *cmc;
	struct resource *res;
	int i = 0;
	int ret = 0;

	xocl_info(pdev, "probing...");

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
		xocl_err(cmc->pdev, "not all needed resources are found");
		ret = -EINVAL;
		goto done;
	}

	ret = cmc_ctrl_probe(cmc->pdev, cmc->regs, &cmc->ctrl_hdl);
	if (ret)
		goto done;

	return 0;

done:
	(void) cmc_remove(pdev);
	return ret;
}

static int
cmc_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	xocl_info(pdev, "handling IOCTL cmd: %d", cmd);
	return 0;
}

static int cmc_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xocl_devnode_open(inode);

	/* Device may have gone already when we get here. */
	if (!pdev)
		return -ENODEV;

	xocl_info(pdev, "opened");
	file->private_data = platform_get_drvdata(pdev);
	return 0;
}

static int cmc_close(struct inode *inode, struct file *file)
{
	struct xocl_cmc *cmc = file->private_data;

	xocl_devnode_close(inode);
	xocl_info(cmc->pdev, "closed");
	return 0;
}

struct xocl_subdev_endpoints xocl_cmc_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names []) {
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

struct xocl_subdev_drvdata xocl_cmc_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = cmc_leaf_ioctl,
	},
	.xsd_file_ops = {
		.xsf_ops = {
			.owner = THIS_MODULE,
			.open = cmc_open,
			.release = cmc_close,
		},
		.xsf_dev_name = "xmc",
	},
};

static const struct platform_device_id cmc_id_table[] = {
	{ XOCL_CMC, (kernel_ulong_t)&xocl_cmc_data },
	{ },
};

struct platform_driver xocl_cmc_driver = {
	.driver	= {
		.name    = XOCL_CMC,
	},
	.probe   = cmc_probe,
	.remove  = cmc_remove,
	.id_table = cmc_id_table,
};
