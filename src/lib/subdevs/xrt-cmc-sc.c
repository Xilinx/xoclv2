// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#include <linux/uaccess.h>
#include "xocl-subdev.h"
#include "xocl-cmc-impl.h"

#define	CMC_CORE_SUPPORT_NOTUPGRADABLE	0x0c010004

enum sc_mode {
	CMC_SC_UNKNOWN = 0,
	CMC_SC_NORMAL,
	CMC_SC_BSL_MODE_UNSYNCED,
	CMC_SC_BSL_MODE_SYNCED,
	CMC_SC_BSL_MODE_SYNCED_SC_NOT_UPGRADABLE,
	CMC_SC_NORMAL_MODE_SC_NOT_UPGRADABLE
};

struct cmc_pkt_payload_image_end {
	u32 BSL_jump_addr;
};

struct cmc_pkt_payload_sector_start {
	u32 addr;
	u32 size;
	u8 data[1];
};

struct cmc_pkt_payload_sector_data {
	u8 data[1];
};

struct xocl_cmc_sc {
	struct platform_device *pdev;
	struct cmc_reg_map reg_io;
	bool sc_fw_erased;
	int mbx_generation;
	size_t mbx_max_payload_sz;
};

static inline void cmc_io_wr(struct xocl_cmc_sc *cmc_sc, u32 off, u32 val)
{
	iowrite32(val, cmc_sc->reg_io.crm_addr + off);
}

static inline u32 cmc_io_rd(struct xocl_cmc_sc *cmc_sc, u32 off)
{
	return ioread32(cmc_sc->reg_io.crm_addr + off);
}

static bool is_sc_ready(struct xocl_cmc_sc *cmc_sc, bool quiet)
{
	union cmc_status st;

	st.status_val = cmc_io_rd(cmc_sc, CMC_REG_IO_STATUS);
	if (st.status.sc_mode == CMC_SC_NORMAL)
		return true;

	if (!quiet) {
		xocl_err(cmc_sc->pdev, "SC is not ready, state=%d",
			st.status.sc_mode);
	}
	return false;
}

static bool is_sc_fixed(struct xocl_cmc_sc *cmc_sc)
{
	union cmc_status st;
	u32 cmc_core_version = cmc_io_rd(cmc_sc, CMC_REG_IO_CORE_VERSION);

	st.status_val = cmc_io_rd(cmc_sc, CMC_REG_IO_STATUS);

	if (cmc_core_version >= CMC_CORE_SUPPORT_NOTUPGRADABLE &&
	    !st.status.invalid_sc &&
	    (st.status.sc_mode == CMC_SC_BSL_MODE_SYNCED_SC_NOT_UPGRADABLE ||
	     st.status.sc_mode == CMC_SC_NORMAL_MODE_SC_NOT_UPGRADABLE))
		return true;

	return false;
}

static int cmc_erase_sc_firmware(struct xocl_cmc_sc *cmc_sc)
{
	int ret = 0;

	if (cmc_sc->sc_fw_erased)
		return 0;

	xocl_info(cmc_sc->pdev, "erasing SC firmware...");
	ret = cmc_mailbox_send_packet(cmc_sc->pdev, cmc_sc->mbx_generation,
		CMC_MBX_PKT_OP_MSP432_ERASE_FW, NULL, 0);
	if (ret == 0)
		cmc_sc->sc_fw_erased = true;
	return ret;
}

static int cmc_write_sc_firmware_section(struct xocl_cmc_sc *cmc_sc,
	loff_t start, size_t n, const char *buf)
{
	int ret = 0;
	size_t sz, thissz, pktsize;
	void *pkt;
	struct cmc_pkt_payload_sector_start *start_payload;
	struct cmc_pkt_payload_sector_data *data_payload;
	u8 pkt_op;

	xocl_info(cmc_sc->pdev, "writing %ld bytes @0x%llx", n, start);

	if (n == 0)
		return 0;

	BUG_ON(!cmc_sc->sc_fw_erased);

	pkt = vzalloc(cmc_sc->mbx_max_payload_sz);
	if (!pkt)
		return -ENOMEM;

	for (sz = 0; ret == 0 && sz < n; sz += thissz) {
		if (sz == 0) {
			/* First packet for the section. */
			pkt_op = CMC_MBX_PKT_OP_MSP432_SEC_START;
			start_payload = pkt;
			start_payload->addr = start;
			start_payload->size = n;
			thissz = cmc_sc->mbx_max_payload_sz - offsetof(
				struct cmc_pkt_payload_sector_start, data);
			thissz = min(thissz, n - sz);
			memcpy(start_payload->data, buf + sz, thissz);
			pktsize = thissz + offsetof(
				struct cmc_pkt_payload_sector_start, data);
		} else {
			pkt_op = CMC_MBX_PKT_OP_MSP432_SEC_DATA;
			data_payload = pkt;
			thissz = cmc_sc->mbx_max_payload_sz - offsetof(
				struct cmc_pkt_payload_sector_data, data);
			thissz = min(thissz, n - sz);
			memcpy(data_payload->data, buf + sz, thissz);
			pktsize = thissz + offsetof(
				struct cmc_pkt_payload_sector_data, data);
		}
		ret = cmc_mailbox_send_packet(cmc_sc->pdev,
			cmc_sc->mbx_generation, pkt_op, pkt, pktsize);
	}

	return ret;
}

static int
cmc_boot_sc(struct xocl_cmc_sc *cmc_sc, u32 jump_addr)
{
	int ret = 0;
	struct cmc_pkt_payload_image_end pkt = { 0 };

	xocl_info(cmc_sc->pdev, "rebooting SC @0x%x", jump_addr);

	BUG_ON(!cmc_sc->sc_fw_erased);

	/* Mark new SC firmware is installed. */
	cmc_sc->sc_fw_erased = false;

	/* Try booting it up. */
	pkt.BSL_jump_addr = jump_addr;
	ret = cmc_mailbox_send_packet(cmc_sc->pdev, cmc_sc->mbx_generation,
		CMC_MBX_PKT_OP_MSP432_IMAGE_END, (char *)&pkt, sizeof(pkt));
	if (ret)
		return ret;

	/* Wait for SC to reboot */
	CMC_LONG_WAIT(is_sc_ready(cmc_sc, true));
	if (!is_sc_ready(cmc_sc, false))
		ret = -ETIMEDOUT;

	return ret;
}

/*
 * Write SC firmware image data at specified location.
 */
ssize_t cmc_update_sc_firmware(struct file *file,
	const char __user *ubuf, size_t n, loff_t *off)
{
	u32 jump_addr = 0;
	struct xocl_cmc_sc *cmc_sc = file->private_data;
	/* Special offset for writing SC's BSL jump address. */
	const loff_t jump_offset = 0xffffffff;
	ssize_t ret = 0;
	u8 *kbuf;
	bool need_refresh = false;

	/* Sanity check input 'n' */
	if (n == 0 || n > jump_offset || n > 100 * 1024 * 1024)
		return -EINVAL;

	kbuf = vmalloc(n);
	if (kbuf == NULL)
		return -ENOMEM;
	if (copy_from_user(kbuf, ubuf, n)) {
		vfree(kbuf);
		return -EFAULT;
	}

	cmc_sc->mbx_generation = cmc_mailbox_acquire(cmc_sc->pdev);
	if (cmc_sc->mbx_generation < 0) {
		vfree(kbuf);
		return -ENODEV;
	}

	ret = cmc_erase_sc_firmware(cmc_sc);
	if (ret) {
		xocl_err(cmc_sc->pdev, "can't erase SC firmware");
	} else if (*off == jump_offset) {
		/*
		 * Write to jump_offset will cause a reboot of SC and jump
		 * to address that is passed in.
		 */
		if (n != sizeof(jump_addr)) {
			xocl_err(cmc_sc->pdev, "invalid jump addr size");
			ret = -EINVAL;
		} else {
			jump_addr = *(u32 *)kbuf;
			ret = cmc_boot_sc(cmc_sc, jump_addr);
			/* Need to reload board info after SC image update */
			need_refresh = true;
		}
	} else {
		ret = cmc_write_sc_firmware_section(cmc_sc, *off, n, kbuf);
	}

	cmc_mailbox_release(cmc_sc->pdev, cmc_sc->mbx_generation);

	if (need_refresh)
		(void) cmc_refresh_board_info(cmc_sc->pdev);

	vfree(kbuf);
	if (ret) {
		cmc_sc->sc_fw_erased = false;
		return ret;
	}

	*off += n;
	return n;
}

/*
 * Only allow one client at a time.
 */
int cmc_sc_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = xocl_devnode_open_excl(inode);

	file->private_data = cmc_pdev2sc(pdev);
	return 0;
}

int cmc_sc_close(struct inode *inode, struct file *file)
{
	struct xocl_cmc_sc *cmc_sc = file->private_data;

	if (!cmc_sc)
		return -EINVAL;

	file->private_data = NULL;
	xocl_devnode_close(inode);
	return 0;
}

loff_t cmc_sc_llseek(struct file *filp, loff_t off, int whence)
{
	loff_t npos;

	switch (whence) {
	case 0: /* SEEK_SET */
		npos = off;
		break;
	case 1: /* SEEK_CUR */
		npos = filp->f_pos + off;
		break;
	case 2: /* SEEK_END: no need to support */
		return -EINVAL;
	default: /* should not happen */
		return -EINVAL;
	}
	if (npos < 0)
		return -EINVAL;

	filp->f_pos = npos;
	return npos;
}

static ssize_t sc_is_fixed_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct xocl_cmc_sc *cmc_sc = cmc_pdev2sc(pdev);

	return sprintf(buf, "%d\n", is_sc_fixed(cmc_sc));
}
static DEVICE_ATTR_RO(sc_is_fixed);

static ssize_t sc_presence_show(struct device *dev,
	struct device_attribute *da, char *buf)
{
	return sprintf(buf, "1\n");
}
static DEVICE_ATTR_RO(sc_presence);

static struct attribute *cmc_sc_attrs[] = {
	&dev_attr_sc_is_fixed.attr,
	&dev_attr_sc_presence.attr,
	NULL
};

static struct attribute_group cmc_sc_attr_group = {
	.attrs = cmc_sc_attrs,
};

void cmc_sc_remove(struct platform_device *pdev)
{
	struct xocl_cmc_sc *cmc_sc = cmc_pdev2sc(pdev);

	if (!cmc_sc)
		return;

	sysfs_remove_group(&pdev->dev.kobj, &cmc_sc_attr_group);
}

int cmc_sc_probe(struct platform_device *pdev,
	struct cmc_reg_map *regmaps, void **hdl)
{
	int ret;
	struct xocl_cmc_sc *cmc_sc;

	cmc_sc = devm_kzalloc(DEV(pdev), sizeof(*cmc_sc), GFP_KERNEL);
	if (!cmc_sc)
		return -ENOMEM;

	cmc_sc->pdev = pdev;
	/* Obtain register maps we need to start/stop CMC. */
	cmc_sc->reg_io = regmaps[IO_REG];
	cmc_sc->mbx_max_payload_sz = cmc_mailbox_max_payload(pdev);
	cmc_sc->mbx_generation = -ENODEV;

	ret = sysfs_create_group(&pdev->dev.kobj, &cmc_sc_attr_group);
	if (ret) {
		xocl_err(pdev, "create sc attrs failed: %d", ret);
		goto fail;
	}

	*hdl = cmc_sc;
	return 0;

fail:
	cmc_sc_remove(pdev);
	devm_kfree(DEV(pdev), cmc_sc);
	return ret;
}
