// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo FPGA ICAP Driver
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
#include "xocl-icap.h"
#include "xocl-xclbin.h"

#define XOCL_ICAP "xocl_icap"

#define	ICAP_ERR(icap, fmt, arg...)	\
	xocl_err((icap)->pdev, fmt "\n", ##arg)
#define	ICAP_WARN(icap, fmt, arg...)	\
	xocl_warn((icap)->pdev, fmt "\n", ##arg)
#define	ICAP_INFO(icap, fmt, arg...)	\
	xocl_info((icap)->pdev, fmt "\n", ##arg)
#define	ICAP_DBG(icap, fmt, arg...)	\
	xocl_dbg((icap)->pdev, fmt "\n", ##arg)

/*
 * AXI-HWICAP IP register layout
 */
struct icap_reg {
	u32	ir_rsvd1[7];
	u32	ir_gier;
	u32	ir_isr;
	u32	ir_rsvd2;
	u32	ir_ier;
	u32	ir_rsvd3[53];
	u32	ir_wf;
	u32	ir_rf;
	u32	ir_sz;
	u32	ir_cr;
	u32	ir_sr;
	u32	ir_wfv;
	u32	ir_rfo;
	u32	ir_asr;
} __packed;

struct icap {
	struct platform_device	*pdev;
	struct icap_reg		*icap_regs;
	struct mutex		icap_lock;

	unsigned int		idcode;
};

static inline u32 reg_rd(void __iomem *reg)
{
	if (!reg)
		return -1;

	return ioread32(reg);
}

static inline void reg_wr(void __iomem *reg, u32 val)
{
	if (!reg)
		return;

	iowrite32(val, reg);
}

static int wait_for_done(struct icap *icap)
{
	u32	w;
	int	i = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));
	for (i = 0; i < 10; i++) {
		udelay(5);
		w = reg_rd(&icap->icap_regs->ir_sr);
		ICAP_INFO(icap, "XHWICAP_SR: %x", w);
		if (w & 0x5)
			return 0;
	}

	ICAP_ERR(icap, "bitstream download timeout");
	return -ETIMEDOUT;
}

static int icap_write(struct icap *icap, const u32 *word_buf, int size)
{
	int i;
	u32 value = 0;

	for (i = 0; i < size; i++) {
		value = be32_to_cpu(word_buf[i]);
		reg_wr(&icap->icap_regs->ir_wf, value);
	}

	reg_wr(&icap->icap_regs->ir_cr, 0x1);

	for (i = 0; i < 20; i++) {
		value = reg_rd(&icap->icap_regs->ir_cr);
		if ((value & 0x1) == 0)
			return 0;
		ndelay(50);
	}

	ICAP_ERR(icap, "writing %d dwords timeout", size);
	return -EIO;
}

static int bitstream_helper(struct icap *icap, const u32 *word_buffer,
	u32 word_count)
{
	u32 remain_word;
	u32 word_written = 0;
	int wr_fifo_vacancy = 0;
	int err = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));
	for (remain_word = word_count; remain_word > 0;
		remain_word -= word_written, word_buffer += word_written) {
		wr_fifo_vacancy = reg_rd(&icap->icap_regs->ir_wfv);
		if (wr_fifo_vacancy <= 0) {
			ICAP_ERR(icap, "no vacancy: %d", wr_fifo_vacancy);
			err = -EIO;
			break;
		}
		word_written = (wr_fifo_vacancy < remain_word) ?
			wr_fifo_vacancy : remain_word;
		if (icap_write(icap, word_buffer, word_written) != 0) {
			ICAP_ERR(icap, "write failed remain %d, written %d",
					remain_word, word_written);
			err = -EIO;
			break;
		}
	}

	return err;
}

static int icap_download(struct icap *icap, const char *buffer,
	unsigned long length)
{
	u32	numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;
	u32	byte_read;
	int	err = 0;

	mutex_lock(&icap->icap_lock);
	for (byte_read = 0; byte_read < length; byte_read += numCharsRead) {
		numCharsRead = length - byte_read;
		if (numCharsRead > DMA_HWICAP_BITFILE_BUFFER_SIZE)
			numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;

		err = bitstream_helper(icap, (u32 *)buffer,
			numCharsRead / sizeof(u32));
		if (err)
			goto failed;
		buffer += numCharsRead;
	}

	err = wait_for_done(icap);

failed:
	mutex_unlock(&icap->icap_lock);

	return err;
}

/*
 * Run the following sequence of canned commands to obtain IDCODE of the FPGA
 */
static void icap_probe_chip(struct icap *icap)
{
	u32 w;

	w = reg_rd(&icap->icap_regs->ir_sr);
	w = reg_rd(&icap->icap_regs->ir_sr);
	reg_wr(&icap->icap_regs->ir_gier, 0x0);
	w = reg_rd(&icap->icap_regs->ir_wfv);
	reg_wr(&icap->icap_regs->ir_wf, 0xffffffff);
	reg_wr(&icap->icap_regs->ir_wf, 0xaa995566);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	reg_wr(&icap->icap_regs->ir_wf, 0x28018001);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	w = reg_rd(&icap->icap_regs->ir_cr);
	reg_wr(&icap->icap_regs->ir_cr, 0x1);
	w = reg_rd(&icap->icap_regs->ir_cr);
	w = reg_rd(&icap->icap_regs->ir_cr);
	w = reg_rd(&icap->icap_regs->ir_sr);
	w = reg_rd(&icap->icap_regs->ir_cr);
	w = reg_rd(&icap->icap_regs->ir_sr);
	reg_wr(&icap->icap_regs->ir_sz, 0x1);
	w = reg_rd(&icap->icap_regs->ir_cr);
	reg_wr(&icap->icap_regs->ir_cr, 0x2);
	w = reg_rd(&icap->icap_regs->ir_rfo);
	icap->idcode = reg_rd(&icap->icap_regs->ir_rf);
	w = reg_rd(&icap->icap_regs->ir_cr);
}

static int
xocl_icap_leaf_ioctl(struct platform_device *pdev, u32 cmd, void *arg)
{
	struct xocl_icap_ioctl_wr	*wr_arg = arg;
	struct icap			*icap;
	int				ret = 0;

	icap = platform_get_drvdata(pdev);

	switch (cmd) {
	case XOCL_ICAP_WRITE:
		ret = icap_download(icap, wr_arg->xiiw_bit_data,
				wr_arg->xiiw_data_len);
		break;
	default:
		ICAP_ERR(icap, "unknown command %d", cmd);
		return -EINVAL;
	}

	return ret;
}

static int xocl_icap_remove(struct platform_device *pdev)
{
	struct icap	*icap;

	icap = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, icap);

	return 0;
}

static int xocl_icap_probe(struct platform_device *pdev)
{
	struct icap	*icap;
	int			ret = 0;
	struct resource		*res;

	icap = devm_kzalloc(&pdev->dev, sizeof(*icap), GFP_KERNEL);
	if (!icap)
		return -ENOMEM;

	icap->pdev = pdev;
	platform_set_drvdata(pdev, icap);
	mutex_init(&icap->icap_lock);

	xocl_info(pdev, "probing");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res != NULL) {
		icap->icap_regs = ioremap(res->start,
			res->end - res->start + 1);
		if (!icap->icap_regs) {
			xocl_err(pdev, "map base failed %pR", res);
			ret = -EIO;
			goto failed;
		}
	}

	icap_probe_chip(icap);
failed:
	return ret;
}

struct xocl_subdev_endpoints xocl_icap_endpoints[] = {
	{
		.xse_names = (struct xocl_subdev_ep_names[]) {
			{ .ep_name = NODE_FPGA_CONFIG },
			{ NULL },
		},
		.xse_min_ep = 1,
	},
	{ 0 },
};

struct xocl_subdev_drvdata xocl_icap_data = {
	.xsd_dev_ops = {
		.xsd_ioctl = xocl_icap_leaf_ioctl,
	},
};

static const struct platform_device_id xocl_icap_table[] = {
	{ XOCL_ICAP, (kernel_ulong_t)&xocl_icap_data },
	{ },
};

struct platform_driver xocl_icap_driver = {
	.driver = {
		.name = XOCL_ICAP,
	},
	.probe = xocl_icap_probe,
	.remove = xocl_icap_remove,
	.id_table = xocl_icap_table,
};
