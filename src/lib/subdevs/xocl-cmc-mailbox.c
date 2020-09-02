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

/* We have a 4k buffer for cmc mailbox */
#define	CMC_PKT_MAX_SZ	1024 /* In u32 */
#define	CMC_PKT_MAX_PAYLOAD_SZ	\
	(CMC_PKT_MAX_SZ - sizeof(struct cmc_pkt_hdr) / sizeof(u32)) /* In u32 */
#define	CMC_PKT_MAX_PAYLOAD_SZ_IN_BYTES	(CMC_PKT_MAX_PAYLOAD_SZ * sizeof(u32))
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

static inline bool
cmc_pkt_host_owned(struct xocl_cmc_mbx *cmc_mbx)
{
	return (cmc_io_rd(cmc_mbx, CMC_REG_IO_CONTROL) &
		CMC_CTRL_MASK_MBX_PKT_OWNER) == 0;
}

static inline void
cmc_pkt_control_set(struct xocl_cmc_mbx *cmc_mbx, u32 ctrl)
{
	u32 val = cmc_io_rd(cmc_mbx, CMC_REG_IO_CONTROL);

	cmc_io_wr(cmc_mbx, CMC_REG_IO_CONTROL, val | ctrl);
}

static inline void
cmc_pkt_notify_device(struct xocl_cmc_mbx *cmc_mbx)
{
	cmc_pkt_control_set(cmc_mbx, CMC_CTRL_MASK_MBX_PKT_OWNER);
}

static inline void
cmc_pkt_clear_error(struct xocl_cmc_mbx *cmc_mbx)
{
	cmc_pkt_control_set(cmc_mbx, CMC_CTRL_MASK_CLR_ERR);
}

static int cmc_mailbox_wait(struct xocl_cmc_mbx *cmc_mbx)
{
	u32 val;

	BUG_ON(!mutex_is_locked(&cmc_mbx->lock));

	CMC_LONG_WAIT(cmc_pkt_host_owned(cmc_mbx));
	if (!cmc_pkt_host_owned(cmc_mbx)) {
		xocl_err(cmc_mbx->pdev, "CMC packet error: time'd out");
		return -ETIMEDOUT;
	}

	val = cmc_io_rd(cmc_mbx, CMC_REG_IO_ERROR);
	if (val & CMC_ERROR_MASK_MBX_ERR)
		val = cmc_io_rd(cmc_mbx, CMC_REG_IO_MBX_ERROR);
	if (val) {
		xocl_err(cmc_mbx->pdev, "CMC packet error: %d", val);
		cmc_pkt_clear_error(cmc_mbx);
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

	BUG_ON(!mutex_is_locked(&cmc_mbx->lock));

#ifdef	MBX_PKT_DEBUG
	xocl_info(cmc_mbx->pdev, "Sending CMC packet: %d DWORDS...", len);
	xocl_info(cmc_mbx->pdev, "opcode=%d payload_sz=0x%x (0x%x)",
		cmc_mbx->pkt.hdr.op, cmc_mbx->pkt.hdr.payload_sz, pkt[0]);
	for (i = 0; i < 16; i++) {// print out first 16 bytes
		/* Comment out to avoid check patch complaint. */
		//pr_cont("%02x ", *((u8 *)(cmc_mbx->pkt.data) + i));
	}
#endif
	/* Push pkt data to mailbox on HW. */
	for (i = 0; i < len; i++) {
		cmc_io_wr(cmc_mbx,
			cmc_mbx->mbx_offset + i * sizeof(u32), pkt[i]);
	}

	/* Notify HW that a pkt is ready for process. */
	cmc_pkt_notify_device(cmc_mbx);
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

	/* Make sure HW is done with the mailbox buffer. */
	ret = cmc_mailbox_wait(cmc_mbx);
	if (ret)
		return ret;

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

	return ret;
}

int cmc_mailbox_recv_packet(struct platform_device *pdev, int generation,
	char *buf, size_t *len)
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
	if (cmc_mbx->pkt.hdr.payload_sz > *len) {
		xocl_err(cmc_mbx->pdev,
			"packet size (0x%x) exceeds buf size (0x%lx)",
			cmc_mbx->pkt.hdr.payload_sz, *len);
		mutex_unlock(&cmc_mbx->lock);
		return -E2BIG;
	}
	memcpy(buf, cmc_mbx->pkt.data, cmc_mbx->pkt.hdr.payload_sz);
	*len = cmc_mbx->pkt.hdr.payload_sz;

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

	if (len > CMC_PKT_MAX_PAYLOAD_SZ_IN_BYTES) {
		xocl_err(cmc_mbx->pdev,
			"packet size (0x%lx) exceeds max size (0x%lx)",
			len, CMC_PKT_MAX_PAYLOAD_SZ_IN_BYTES);
		return -E2BIG;
	}

	mutex_lock(&cmc_mbx->lock);

	memset(&cmc_mbx->pkt, 0, sizeof(struct cmc_pkt));
	cmc_mbx->pkt.hdr.op = op;
	cmc_mbx->pkt.hdr.payload_sz = len;
	if (buf)
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

void cmc_mailbox_release(struct platform_device *pdev, int generation)
{
	struct xocl_cmc_mbx *cmc_mbx = cmc_pdev2mbx(pdev);

	if (cmc_mbx->generation != generation) {
		xocl_err(cmc_mbx->pdev, "stale generation number passed in");
		return;
	}

	/*
	 * A hold is released, bump up generation number
	 * to invalidate the previous hold.
	 */
	cmc_mbx->generation++;
	up(&cmc_mbx->sem);
}

size_t cmc_mailbox_max_payload(struct platform_device *pdev)
{
	return CMC_PKT_MAX_PAYLOAD_SZ_IN_BYTES;
}

void cmc_mailbox_remove(struct platform_device *pdev)
{
	/* Nothing to do */
}

int cmc_mailbox_probe(struct platform_device *pdev,
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
	cmc_mbx->mbx_offset = cmc_io_rd(cmc_mbx, CMC_REG_IO_MBX_OFFSET);
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
	cmc_mailbox_remove(pdev);
	devm_kfree(DEV(pdev), cmc_mbx);
	return -ENODEV;
}
