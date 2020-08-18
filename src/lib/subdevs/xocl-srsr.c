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

#define XOCL_DDR_SRSR "xocl_ddr_srsr"

#define	REG_STATUS_OFFSET		0x00000000
#define	REG_CTRL_OFFSET			0x00000004
#define	REG_CALIB_OFFSET		0x00000008
#define	REG_XSDB_RAM_BASE		0x00004000

#define	FULL_CALIB_TIMEOUT		100
#define	FAST_CALIB_TIMEOUT		15

#define	CTRL_BIT_SYS_RST		0x00000001
#define	CTRL_BIT_XSDB_SELECT		0x00000010
#define	CTRL_BIT_MEM_INIT_SKIP		0x00000020
#define	CTRL_BIT_RESTORE_EN		0x00000040
#define	CTRL_BIT_RESTORE_COMPLETE	0x00000080
#define	CTRL_BIT_SREF_REQ		0x00000100

#define	STATUS_BIT_CALIB_COMPLETE	0x00000001
#define	STATUS_BIT_SREF_ACK		0x00000100

struct xocl_ddr_srsr {
	void __iomem		*base;
	struct device		*dev;
	struct mutex		lock;
	uint32_t		*calib_cache;
	uint32_t		cache_size;
	bool			restored;
};

#define reg_rd(g, offset)	ioread32(g->base + offset)
#define reg_wr(g, val, offset)	iowrite32(val, g->base + offset)

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	u32 status = 1;

	return sprintf(buf, "0x%x\n", status);
}
static DEVICE_ATTR_RO(status);

static struct attribute *xocl_ddr_srsr_attributes[] = {
	&dev_attr_status.attr,
	NULL
};

static const struct attribute_group xocl_ddr_srsr_attrgroup = {
	.attrs = xocl_ddr_srsr_attributes,
};

static int srsr_full_calibration(struct platform_device *pdev)
{
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);
	int i = 0, err = -ETIMEDOUT;
	u32 val;

	reg_wr(srsr, CTRL_BIT_SYS_RST, REG_CTRL_OFFSET);
	reg_wr(srsr, 0x0, REG_CTRL_OFFSET);


	/* Safe to say, full calibration should finish in 2000ms*/
	for (; i < FULL_CALIB_TIMEOUT; ++i) {
		val = reg_rd(srsr,REG_STATUS_OFFSET);
		if (val & STATUS_BIT_CALIB_COMPLETE) {
			err = 0;
			break;
		}
		msleep(20);
	}
	xocl_info(pdev, "calibrate time %d", i * 20);

	return err;
}

static int srsr_save_calib(struct platform_device *pdev)
{
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);
	int i = 0, err = -ETIMEDOUT;
	u32 val = 0;
	u32 cache_size_in_words = srsr->cache_size/sizeof(uint32_t);

	if(!srsr->calib_cache)
		return err;

	mutex_lock(&srsr->lock);
	reg_wr(srsr, CTRL_BIT_SREF_REQ, REG_CTRL_OFFSET);
	for ( ; i < 20; ++i) {
		val = reg_rd(srsr, REG_STATUS_OFFSET);
		if (val == (STATUS_BIT_SREF_ACK|STATUS_BIT_CALIB_COMPLETE)) {
			err = 0;
			break;
		}
		msleep(20);
	}

	reg_wr(srsr, CTRL_BIT_SREF_REQ | CTRL_BIT_XSDB_SELECT, REG_CTRL_OFFSET);

	for (i = 0; i < cache_size_in_words; ++i) {
		val = reg_rd(srsr, REG_XSDB_RAM_BASE+i*4);
		*(srsr->calib_cache+i) = val;
	}

	mutex_unlock(&srsr->lock);
	return err;
}

static int srsr_fast_calib(struct platform_device *pdev, bool retention)
{
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);
	int i = 0, err = -ETIMEDOUT;
	u32 val, write_val = CTRL_BIT_RESTORE_EN | CTRL_BIT_XSDB_SELECT;
	u32 cache_size_in_words = xocl_ddr_srsr->cache_size/sizeof(uint32_t);

	if(!srsr->calib_cache)
		return err;

	if (retention)
		write_val |= CTRL_BIT_MEM_INIT_SKIP;

	reg_wr(srsr, write_val, REG_CTRL_OFFSET);

	msleep(20);
	for (i = 0; i < cache_size_in_words; ++i) {
		val = *(srsr->calib_cache+i);
		reg_wr(srsr, val, REG_XSDB_RAM_BASE+i*4);
	}

	write_val = CTRL_BIT_RESTORE_EN | CTRL_BIT_RESTORE_COMPLETE;
	if (retention)
		write_val |= CTRL_BIT_MEM_INIT_SKIP;

	reg_wr(srsr, write_val, REG_CTRL_OFFSET);

	/* Safe to say, fast calibration should finish in 300ms*/
	for (i = 0; i < FAST_CALIB_TIMEOUT; ++i) {
		val = reg_rd(srsr, REG_STATUS_OFFSET);
		if (val & STATUS_BIT_CALIB_COMPLETE) {
			err = 0;
			break;
		}
		msleep(20);
	}
	xocl_info("fast calibration time %d", i * 20);

	reg_wr(srsr, CTRL_BIT_RESTORE_COMPLETE, REG_CTRL_OFFSET);
	val = reg_rd(srsr, REG_CTRL_OFFSET);
	return err;
}

static int srsr_calib(struct platform_device *pdev, bool retention)
{
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);
	int err = -1;
	uint32_t addr0, addr1;

	mutex_lock(&srsr->lock);

	if (srsr->restored)
		err = srsr_fast_calib(pdev, retention);

	/* Fast calibration fails then fall back to full calibration
	 * Wipe out calibration cache before full calibration
	 */
	if (err) {
		err = srsr_full_calibration(pdev);
		if (err)
			goto done;

		/* END_ADDR0/1 provides the end address for a given memory
		 * configuration
		 * END_ADDR 0 is lower 9 bits, the other one is higher 9 bits
		 * E.g. addr0 = 0x155,     0'b 1 0101 0101
		 *      addr1 = 0x5    0'b 0101
		 *                     0'b 01011 0101 0101
		 *                   =  0xB55
		 * and the total size is 0xB55+1
		 * Check the value, it should not excess predefined XSDB range
		 */
		addr0 = reg_rd(srsr, REG_XSDB_RAM_BASE+4);
		addr1 = reg_rd(srsr, REG_XSDB_RAM_BASE+8);

		srsr->cache_size = (((addr1 << 9) | addr0)+1)*sizeof(uint32_t);
		if (srsr->cache_size >= 0x4000) {
			err = -ENOMEM;
			goto done;
		}
		vfree(srsr->calib_cache);
		srsr->calib_cache = vzalloc(srsr->cache_size);
		if (!srsr->calib_cache) {
			err = -ENOMEM;
			goto done;
		}
	}
done:
	mutex_unlock(&srsr->lock);
	return err;
}

static int srsr_read_calib(struct platform_device *pdev, void *calib_cache,
	uint32_t size)
{
	int ret = 0;
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);

	BUG_ON(!srsr->calib_cache);
	BUG_ON(!calib_cache);
	BUG_ON(size != srsr->cache_size);

	mutex_lock(&srsr->lock);
	memcpy(calib_cache, srsr->calib_cache, size);
	mutex_unlock(&srsr->lock);
	return ret;
}

static int srsr_write_calib(struct platform_device *pdev,
	const void *calib_cache, uint32_t size)
{
	int ret = 0, err = 0;
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);

	BUG_ON(!calib_cache);

	mutex_lock(&srsr->lock);
	srsr->cache_size = size;

	vfree(srsr->calib_cache);
	srsr->calib_cache = vzalloc(srsr->cache_size);
	if (!srsr->calib_cache) {
		err = -ENOMEM;
		goto done;
	}

	memcpy(srsr->calib_cache, calib_cache, size);

	srsr->restored = true;
done:
	mutex_unlock(&srsr->lock);
	return ret;
}

static uint32_t srsr_cache_size(struct platform_device *pdev)
{
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);

	return srsr->cache_size;
}

static int
xocl_srsr_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	int ret;

	switch (cmd) {
	case XOCL_DDR_SRSR_SAVE:
		ret = srsr_save_calib(pdev);
		break;
	case XOCL_DDR_SRSR_CALIB:
		ret = srsr_calib(pdev, (bool)arg);
		break;
	case XOCL_DDR_SRSR_WRITE: {
		struct xocl_srsr_ioctl_rw *wr_arg = arg;

		ret = srsr_write_calib(pdev, wr_arg->xdirw_buf,
			wr_arg->xdirw_size);
		break;
	}
	case XOCL_DDR_SRSR_READ: {
		struct xocl_srsr_ioctl_rw *rd_arg = arg;

		ret = srsr_read_calib(pdev, rd_arg->xdirw_buf,
			rd_arg->xdirw_size);
		break;
	}
	default:
		xocl_err(pdev, "unsupported cmd %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xocl_srsr_probe(struct platform_device *pdev)
{
	struct xocl_ddr_srsr *srsr;
	struct resource *res;
	int err = 0;

	srsr = devm_kzalloc(&pdev->dev, sizeof(*xocl_ddr_srsr), GFP_KERNEL);
	if (!srsr)
		return -ENOMEM;

	srsr->pdev = pdev;
	platform_set_drvdata(pdev, srsr);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		goto failed;

	xocl_info(pdev, "IO start: 0x%llx, end: 0x%llx",
		res->start, res->end);

	srsr->base = ioremap(res->start, res->end - res->start + 1);
	if (!srsr->base) {
		err = -EIO;
		xocl_err(pdev, "Map iomem failed");
		goto failed;
	}
	mutex_init(&srsr->lock);

	err = sysfs_create_group(&pdev->dev.kobj, &xocl_ddr_srsr_attrgroup);
	if (err)
		goto create_xocl_ddr_srsr_failed;

	return 0;

create_xocl_ddr_srsr_failed:
	platform_set_drvdata(pdev, NULL);
failed:
	return err;
}

static int xocl_srsr_remove(struct platform_device *pdev)
{
	struct xocl_ddr_srsr *srsr = platform_get_drvdata(pdev);

	if (!srsr) {
		xocl_err(pdev, "driver data is NULL");
		return -EINVAL;
	}

	sysfs_remove_group(&pdev->dev.kobj, &xocl_ddr_srsr_attrgroup);

	if (srsr->base)
		iounmap(srsr->base);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, srsr);

	return 0;
}

struct xocl_subdev_endpoints xocl_srsr_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .regmap_name = "ddr_srsr" },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xocl_subdev_drvdata xocl_srsr_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_srsr_leaf_ioctl,
	},
};

static const struct platform_device_id xocl_srsr_table[] = {
	{ XOCL_DDR_SRSR, (kernel_ulong_t)&xocl_srsr_data },
	{ },
};

struct platform_driver xocl_ddr_srsr_driver = {
	.driver = {
		.name = XOCL_DDR_SRSR,
	},
	.probe = xocl_srsr_probe,
	.remove = xocl_srsr_remove,
	.id_table = xocl_srsr_table,
};
