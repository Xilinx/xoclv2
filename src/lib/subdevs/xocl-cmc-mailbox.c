// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/mutex.h>
#include <linux/delay.h>
#include "xocl-subdev.h"
#include "xocl-cmc-impl.h"

#define	CMC_ERROR_REG				0xc
#define	CMC_CONTROL_REG				0x18
#define	CMC_HOST_MSG_OFFSET_REG			0x300
#define	CMC_HOST_MSG_ERROR_REG			0x304

#define	CMC_PKT_OWNER_MASK			(1 << 5)
#define	CMC_PKT_ERR_MASK			(1 << 26)
#define	CMC_CTRL_ERR_CLR_MASK			(1 << 1)

#define	XMC_HOST_MSG_NO_ERR			0x00
#define	XMC_HOST_MSG_BAD_OPCODE_ERR		0x01
#define	XMC_HOST_MSG_UNKNOWN_ERR		0x02
#define	XMC_HOST_MSG_MSP432_MODE_ERR		0x03
#define	XMC_HOST_MSG_MSP432_FW_LENGTH_ERR	0x04
#define	XMC_HOST_MSG_BRD_INFO_MISSING_ERR	0x05

/* We have a 4k buffer for cmc mailbox */
#define	CMC_PKT_MAX_SZ	1024 /* In u32 */
#define	CMC_PKT_MAX_PAYLOAD_SZ	\
	(CMC_PKT_MAX_SZ - sizeof(struct cmc_pkt_hdr) / sizeof(u32)) /* In u32 */
#define	CMC_PKT_SZ(hdr)		\
	((sizeof(struct cmc_pkt_hdr) + (hdr)->payload_sz + sizeof(u32) - 1) / \
	sizeof(u32)) /* In u32 */

/* Make sure hdr is multiple of u32 */
struct cmc_pkt_hdr {
	u32 payload_sz	: 12;
	u32 reserved	: 12;
	u32 op		: 8;
};

struct cmc_pkt {
	struct cmc_pkt_hdr hdr;
	u32 data[CMC_PKT_MAX_PAYLOAD_SZ];
};

struct xocl_cmc_mbx {
	struct platform_device *pdev;
	struct cmc_reg_map reg_io;
	u32 mbx_offset;
	struct mutex lock;
	struct cmc_pkt pkt;
	struct semaphore sem;
	int generation;
};

static inline void
cmc_io_wr(struct xocl_cmc_mbx *cmc_mbx, u32 off, u32 val)
{
	iowrite32(val, cmc_mbx->reg_io.crm_addr + off);
}

static inline u32
cmc_io_rd(struct xocl_cmc_mbx *cmc_mbx, u32 off)
{
	return ioread32(cmc_mbx->reg_io.crm_addr + off);
}

static int cmc_mailbox_wait(struct xocl_cmc_mbx *cmc_mbx)
{
	int retry = CMC_MAX_RETRY * 4;
	u32 val;

	BUG_ON(!mutex_is_locked(&cmc_mbx->lock));

	val = cmc_io_rd(cmc_mbx, CMC_CONTROL_REG);
	while ((retry > 0) && (val & CMC_PKT_OWNER_MASK)) {
		msleep(CMC_RETRY_INTERVAL);
		val = cmc_io_rd(cmc_mbx, CMC_CONTROL_REG);
		retry--;
	}

	if (retry == 0) {
		xocl_err(cmc_mbx->pdev, "CMC packet error: time'd out");
		return -ETIMEDOUT;
	}

	val = cmc_io_rd(cmc_mbx, CMC_ERROR_REG);
	if (val & CMC_PKT_ERR_MASK)
		val = cmc_io_rd(cmc_mbx, CMC_HOST_MSG_ERROR_REG);
	if (val) {
		xocl_err(cmc_mbx->pdev, "CMC packet error: %d", val);
		val = cmc_io_rd(cmc_mbx, CMC_CONTROL_REG);
		cmc_io_wr(cmc_mbx, CMC_CONTROL_REG, val|CMC_CTRL_ERR_CLR_MASK);
		return -EIO;
	}

	return 0;
}

static int cmc_mailbox_pkt_write(struct xocl_cmc_mbx *cmc_mbx)
{
	u32 *pkt = (u32 *)&cmc_mbx->pkt;
	u32 len = CMC_PKT_SZ(&cmc_mbx->pkt.hdr);
	int ret = 0;
	u32 i;
	u32 val;

	BUG_ON(!mutex_is_locked(&cmc_mbx->lock));

#ifdef	MBX_PKT_DEBUG
	xocl_info(cmc_mbx->pdev, "Sending CMC packet: %d DWORDS...", len);
	xocl_info(cmc_mbx->pdev, "opcode=%d payload_sz=0x%x (0x%x)",
		cmc_mbx->pkt.hdr.op, cmc_mbx->pkt.hdr.payload_sz, pkt[0]);
	for (i = 0; i < 16; i++) {// print out first 16 bytes
		xocl_cont(cmc_mbx->pdev, "%02x ",
			*((u8 *)(cmc_mbx->pkt.data) + i));
	}
#endif

	/* Push pkt data to mailbox on HW. */
	for (i = 0; i < len; i++) {
		cmc_io_wr(cmc_mbx, cmc_mbx->mbx_offset + i * sizeof(u32),
			pkt[i]);
	}

	/* Notify HW that a pkt is ready for process. */
	val = cmc_io_rd(cmc_mbx, CMC_CONTROL_REG);
	cmc_io_wr(cmc_mbx, CMC_CONTROL_REG, val | CMC_PKT_OWNER_MASK);

	/* Make sure HW is done with the mailbox buffer. */
	ret = cmc_mailbox_wait(cmc_mbx);

	return ret;
}

static int cmc_mailbox_pkt_read(struct xocl_cmc_mbx *cmc_mbx)
{
	struct cmc_pkt_hdr hdr;
	u32 *pkt;
	u32 len;
	u32 i;
	int ret = 0;

	BUG_ON(!mutex_is_locked(&cmc_mbx->lock));

	/* Receive pkt hdr. */
	pkt = (u32 *)&hdr;
	len = sizeof(hdr) / sizeof(u32);
	for (i = 0; i < len; i++) {
		pkt[i] = cmc_io_rd(cmc_mbx,
			cmc_mbx->mbx_offset + i * sizeof(u32));
	}

	pkt = (u32 *)&cmc_mbx->pkt;
	len = CMC_PKT_SZ(&hdr);
	if (hdr.payload_sz == 0 || len > CMC_PKT_MAX_SZ) {
		xocl_err(cmc_mbx->pdev, "read invalid CMC packet");
		return -EINVAL;
	}

	/* Load pkt data from mailbox on HW. */
	for (i = 0; i < len; i++) {
		pkt[i] = cmc_io_rd(cmc_mbx,
			cmc_mbx->mbx_offset + i * sizeof(u32));
	}

	/* Make sure HW is done with the mailbox buffer. */
	ret = cmc_mailbox_wait(cmc_mbx);
	return ret;
}

int cmc_mailbox_recv_packet(struct platform_device *pdev, int generation,
	char *buf, size_t len)
{
	int ret;
	struct xocl_cmc_mbx *cmc_mbx = cmc_pdev2mbx(pdev);

	if (cmc_mbx == NULL)
		return -EINVAL;

	if (cmc_mbx->generation != generation) {
		xocl_err(cmc_mbx->pdev, "stale generation number passed in");
		return -EINVAL;
	}

	mutex_lock(&cmc_mbx->lock);

	ret = cmc_mailbox_pkt_read(cmc_mbx);
	if (ret) {
		mutex_unlock(&cmc_mbx->lock);
		return ret;
	}
	if (cmc_mbx->pkt.hdr.payload_sz > len) {
		xocl_err(cmc_mbx->pdev,
			"packet size (0x%x) exceeds buf size (0x%lx)",
			cmc_mbx->pkt.hdr.payload_sz, len);
		mutex_unlock(&cmc_mbx->lock);
		return -E2BIG;
	}
	memcpy(buf, cmc_mbx->pkt.data, cmc_mbx->pkt.hdr.payload_sz);

	mutex_unlock(&cmc_mbx->lock);
	return 0;
}

int cmc_mailbox_send_packet(struct platform_device *pdev, int generation,
	u8 op, const char *buf, size_t len)
{
	int ret;
	struct xocl_cmc_mbx *cmc_mbx = cmc_pdev2mbx(pdev);

	if (cmc_mbx == NULL)
		return -ENODEV;

	if (cmc_mbx->generation != generation) {
		xocl_err(cmc_mbx->pdev, "stale generation number passed in");
		return -EINVAL;
	}

	if (len > CMC_PKT_MAX_PAYLOAD_SZ) {
		xocl_err(cmc_mbx->pdev,
			"packet size (0x%lx) exceeds max size (0x%lx)",
			len, CMC_PKT_MAX_PAYLOAD_SZ);
		return -E2BIG;
	}

	mutex_lock(&cmc_mbx->lock);

	cmc_mbx->pkt.hdr.op = op;
	cmc_mbx->pkt.hdr.payload_sz = len;
	memcpy(cmc_mbx->pkt.data, buf, len);
	ret = cmc_mailbox_pkt_write(cmc_mbx);

	mutex_unlock(&cmc_mbx->lock);

	return ret;
}

int cmc_mailbox_acquire(struct platform_device *pdev)
{
	struct xocl_cmc_mbx *cmc_mbx = cmc_pdev2mbx(pdev);

	if (cmc_mbx == NULL)
		return -EINVAL;

	if (down_killable(&cmc_mbx->sem)) {
		xocl_info(cmc_mbx->pdev, "giving up on acquiring CMC mailbox");
		return -ERESTARTSYS;
	}

	return cmc_mbx->generation;
}

void cmc_mailbox_release(struct platform_device *pdev)
{
	struct xocl_cmc_mbx *cmc_mbx = cmc_pdev2mbx(pdev);

	/*
	 * A hold is released, bump up generation number
	 * to invalidate the previous hold.
	 */
	cmc_mbx->generation++;
	up(&cmc_mbx->sem);
}

size_t cmc_mailbox_max_payload(struct platform_device *pdev)
{
	return CMC_PKT_MAX_PAYLOAD_SZ;
}

void cmc_mbx_remove(struct platform_device *pdev)
{
}

int cmc_mbx_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl)
{
	struct xocl_cmc_mbx *cmc_mbx;

	cmc_mbx = devm_kzalloc(DEV(pdev), sizeof(*cmc_mbx), GFP_KERNEL);
	if (!cmc_mbx)
		return -ENOMEM;

	cmc_mbx->pdev = pdev;
	/* Obtain register maps we need to start/stop CMC. */
	cmc_mbx->reg_io = regmaps[IO_REG];
	mutex_init(&cmc_mbx->lock);
	sema_init(&cmc_mbx->sem, 1);
	cmc_mbx->mbx_offset = cmc_io_rd(cmc_mbx, CMC_HOST_MSG_OFFSET_REG);
	if (cmc_mbx->mbx_offset == 0) {
		xocl_err(cmc_mbx->pdev, "CMC mailbox is not available");
		goto done;
	} else {
		xocl_info(cmc_mbx->pdev, "CMC mailbox offset is 0x%x",
			cmc_mbx->mbx_offset);
	}

	*hdl = cmc_mbx;
	return 0;
done:
	cmc_mbx_remove(pdev);
	devm_kfree(DEV(pdev), cmc_mbx);
	return -ENODEV;
}
