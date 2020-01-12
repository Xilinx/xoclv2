// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for Xilinx acclerator FPGA image download feature.
 * Bulk of the code borrowed from XRT driver file icap.c
 *
 * Copyright (C) 2016-2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 *          chien-wei.lan@xilinx.com
 */

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/cred.h>
#include <linux/sched/signal.h>
#include <linux/efi.h>

#include "xocl-icap.h"
#include "xocl-lib.h"
#include "xocl-features.h"
#include "xocl-mailbox-proto.h"
#include "xclbin.h"

static struct key *icap_keys = NULL;

static u32 gate_free_user[] = {0xe, 0xc, 0xe, 0xf};

static struct attribute_group icap_attr_group;

struct xocl_icap {
	struct xocl_subdev_base    core;
	struct mutex		   icap_lock;
	struct icap_reg		  *icap_regs;
	struct icap_generic_state *icap_state;
	unsigned int		   idcode;
	bool			   icap_axi_gate_frozen;
	struct icap_axi_gate	  *icap_axi_gate;

	uuid_t			   icap_bitstream_uuid;
	int			   icap_bitstream_ref;

	char			  *icap_clock_bases[ICAP_MAX_NUM_CLOCKS];
	unsigned short		   icap_ocl_frequency[ICAP_MAX_NUM_CLOCKS];

	struct clock_freq_topology *icap_clock_freq_topology;
	unsigned long		    icap_clock_freq_topology_length;
	char			   *icap_clock_freq_counter;
	struct mem_topology	   *mem_topo;
	struct ip_layout	   *ip_layout;
	struct debug_ip_layout	   *debug_layout;
	struct connectivity	   *connectivity;
	void			   *partition_metadata;

	void			   *rp_bit;
	unsigned long		    rp_bit_len;
	void			   *rp_fdt;
	unsigned long		    rp_fdt_len;
	void			   *rp_mgmt_bin;
	unsigned long		    rp_mgmt_bin_len;
	void			   *rp_sche_bin;
	unsigned long		    rp_sche_bin_len;
	void			   *rp_sc_bin;
	unsigned long		   *rp_sc_bin_len;

	struct bmc		    bmc_header;

	char			   *icap_clock_freq_counters[ICAP_MAX_NUM_CLOCKS];
	char			   *icap_ucs_control_status;

	uint64_t		    cache_expire_secs;
	struct xcl_pr_region	    cache;
	ktime_t			    cache_expires;

	enum icap_sec_level	    sec_level;


	/* Use reader_ref as xclbin metadata reader counter
	 * Ther reference count increases by 1
	 * if icap_xclbin_rd_lock get called.
	 */
	u64			    busy;
	int			    reader_ref;
	wait_queue_head_t	    reader_wq;
};

static int icap_parse_bitstream_axlf_section(struct platform_device *pdev,
	const struct axlf *xclbin, enum axlf_section_kind kind);
static void icap_set_data(struct xocl_icap *icap, struct xcl_pr_region *hwicap);
static uint64_t icap_get_data_nolock(struct platform_device *pdev, enum data_kind kind);
static uint64_t icap_get_data(struct platform_device *pdev, enum data_kind kind);
static const struct axlf_section_header *get_axlf_section_hdr(
	struct xocl_icap *icap, const struct axlf *top, enum axlf_section_kind kind);
static void icap_refresh_addrs(struct platform_device *pdev);

/*
 * Run the following sequence of canned commands to obtain IDCODE of the FPGA
 */
static void icap_probe_chip(struct xocl_icap *icap)
{
	u32 w;

	if (!ICAP_PRIVILEGED(icap))
		return;

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

static int icap_xclbin_wr_lock(struct xocl_icap *icap)
{
	pid_t pid = pid_nr(task_tgid(current));
	int ret = 0;

	mutex_lock(&icap->icap_lock);
	if (icap->busy) {
		ret = -EBUSY;
	} else {
		icap->busy = (u64)pid;
	}
	mutex_unlock(&icap->icap_lock);

 	if (ret)
		goto done;

	ret = wait_event_interruptible(icap->reader_wq, icap->reader_ref == 0);

	if (ret)
		goto done;

	BUG_ON(icap->reader_ref != 0);

done:
	ICAP_DBG(icap, "%d ret: %d", pid, ret);
	return ret;
}
static void icap_xclbin_wr_unlock(struct xocl_icap *icap)
{
	pid_t pid = pid_nr(task_tgid(current));

	BUG_ON(icap->busy != (u64)pid);

	mutex_lock(&icap->icap_lock);
	icap->busy = 0;
	mutex_unlock(&icap->icap_lock);
	ICAP_DBG(icap, "%d", pid);
}
static int icap_xclbin_rd_lock(struct xocl_icap *icap)
{
	pid_t pid = pid_nr(task_tgid(current));
	int ret = 0;

	mutex_lock(&icap->icap_lock);

	if (icap->busy) {
		ret = -EBUSY;
		goto done;
	}

	icap->reader_ref++;

done:
	mutex_unlock(&icap->icap_lock);
	ICAP_DBG(icap, "%d ret: %d", pid, ret);
	return ret;
}
static  void icap_xclbin_rd_unlock(struct xocl_icap *icap)
{
	pid_t pid = pid_nr(task_tgid(current));
	bool wake = false;

	mutex_lock(&icap->icap_lock);

	BUG_ON(icap->reader_ref == 0);

	ICAP_DBG(icap, "%d", pid);

	wake = (--icap->reader_ref == 0);

	mutex_unlock(&icap->icap_lock);
	if (wake)
		wake_up_interruptible(&icap->reader_wq);
}


static void icap_free_bins(struct xocl_icap *icap)
{
	if (icap->rp_bit) {
		vfree(icap->rp_bit);
		icap->rp_bit = NULL;
		icap->rp_bit_len = 0;
	}
	if (icap->rp_fdt) {
		vfree(icap->rp_fdt);
		icap->rp_fdt = NULL;
		icap->rp_fdt_len = 0;
	}
	if (icap->rp_mgmt_bin) {
		vfree(icap->rp_mgmt_bin);
		icap->rp_mgmt_bin = NULL;
		icap->rp_mgmt_bin_len = 0;
	}
	if (icap->rp_sche_bin) {
		vfree(icap->rp_sche_bin);
		icap->rp_sche_bin = NULL;
		icap->rp_sche_bin_len = 0;
	}
}

static long myioctl(struct platform_device *pdev, unsigned int cmd, unsigned long arg)
{
	xocl_info(&pdev->dev, "Subdev %s ioctl %d %ld\n", pdev->name, cmd, arg);
	return 0;
}

static int xocl_post_init_icap(struct xocl_subdev_drv *ops)
{
	int err = 0;
	icap_keys = keyring_alloc(XOCL_AXLF_SIGNING_KEYS, KUIDT_INIT(0),
				  KGIDT_INIT(0), current_cred(),
				  ((KEY_POS_ALL & ~KEY_POS_SETATTR) |
				   KEY_USR_VIEW | KEY_USR_WRITE | KEY_USR_SEARCH),
				  KEY_ALLOC_NOT_IN_QUOTA, NULL, NULL);

	if (IS_ERR(icap_keys)) {
		err = PTR_ERR(icap_keys);
		icap_keys = NULL;
		pr_err("Failed to allocate keyring \"%s\": %d\n",
		       XOCL_AXLF_SIGNING_KEYS, err);
		return err;
	}
	pr_info("Allocated keyring \"%s\" for xclbin signature validation\n",
		XOCL_AXLF_SIGNING_KEYS);
	return 0;
}

static void xocl_pre_exit_icap(struct xocl_subdev_drv *ops)
{
	if (!icap_keys)
		return;

	key_put(icap_keys);
	pr_info("Released keyring \"%s\"\n", XOCL_AXLF_SIGNING_KEYS);
}

static int icap_open(struct inode *inode, struct file *file)
{
	struct xocl_icap *icap = container_of(inode->i_cdev, struct xocl_icap, core.chr_dev);

	if (!icap)
		return -ENXIO;

	file->private_data = icap;
	return 0;
}

static int icap_close(struct inode *inode, struct file *file)
{
	struct xocl_icap *icap = file->private_data;

	file->private_data = NULL;
	return 0;
}

static ssize_t icap_write_rp(struct file *filp, const char __user *data,
			     size_t data_len, loff_t *off)
{
	struct xocl_icap *icap = filp->private_data;
	struct axlf axlf_header = { {0} };
	struct axlf *axlf = NULL;
	const struct axlf_section_header *section;
	return 0;
}

static const struct file_operations icap_fops = {
	.open = icap_open,
	.release = icap_close,
	.write = icap_write_rp,
};

static struct xocl_subdev_drv icap_ops = {
	.ioctl = myioctl,
	.fops = &icap_fops,
	.id = XOCL_SUBDEV_ICAP,
	.dnum = -1,
	.subdrv_post_init = xocl_post_init_icap,
	.subdrv_pre_exit = xocl_pre_exit_icap,
};

static inline void free_clock_freq_topology(struct xocl_icap *icap)
{
	vfree(icap->icap_clock_freq_topology);
	icap->icap_clock_freq_topology = NULL;
	icap->icap_clock_freq_topology_length = 0;
}

static inline void icap_write_clock_freq(struct clock_freq *dst,
					 const struct clock_freq *src)
{
	dst->m_freq_Mhz = src->m_freq_Mhz;
	dst->m_type = src->m_type;
	memcpy(&dst->m_name, &src->m_name, sizeof(src->m_name));
}

static void icap_clean_axlf_section(struct xocl_icap *icap,
				    enum axlf_section_kind kind)
{
	void **target = NULL;

	switch (kind) {
	case IP_LAYOUT:
		target = (void **)&icap->ip_layout;
		break;
	case MEM_TOPOLOGY:
		target = (void **)&icap->mem_topo;
		break;
	case DEBUG_IP_LAYOUT:
		target = (void **)&icap->debug_layout;
		break;
	case CONNECTIVITY:
		target = (void **)&icap->connectivity;
		break;
	case CLOCK_FREQ_TOPOLOGY:
		target = (void **)&icap->icap_clock_freq_topology;
		break;
	case PARTITION_METADATA:
		target = (void **)&icap->partition_metadata;
		break;
	default:
		break;
	}
	if (target) {
		vfree(*target);
		*target = NULL;
	}
}

static void icap_clean_bitstream_axlf(struct xocl_icap *icap)
{
	uuid_copy(&icap->icap_bitstream_uuid, &uuid_null);
	icap_clean_axlf_section(icap, IP_LAYOUT);
	icap_clean_axlf_section(icap, MEM_TOPOLOGY);
	icap_clean_axlf_section(icap, DEBUG_IP_LAYOUT);
	icap_clean_axlf_section(icap, CONNECTIVITY);
	icap_clean_axlf_section(icap, CLOCK_FREQ_TOPOLOGY);
	icap_clean_axlf_section(icap, PARTITION_METADATA);
}

static struct attribute *icap_attrs[] = {
	/*
	&dev_attr_clock_freqs.attr,
	&dev_attr_idcode.attr,
	&dev_attr_cache_expire_secs.attr,
	&dev_attr_sec_level.attr,
	&dev_attr_clock_freqs_max.attr,
	&dev_attr_clock_freqs_min.attr,
	&dev_attr_reader_cnt.attr,
	*/
	NULL,
};

static struct bin_attribute *icap_bin_attrs[] = {
	/*
	&debug_ip_layout_attr,
	&ip_layout_attr,
	&connectivity_attr,
	&mem_topology_attr,
	&rp_bit_attr,
	&clock_freq_topology_attr,
	*/
	NULL,
};

static struct attribute_group icap_attr_group = {
	.attrs = icap_attrs,
	.bin_attrs = icap_bin_attrs,
};

static int xocl_icap_remove(struct platform_device *pdev)
{
	struct xocl_icap *icap = platform_get_drvdata(pdev);
	free_clock_freq_topology(icap);
	sysfs_remove_group(&pdev->dev.kobj, &icap_attr_group);
	iounmap(icap->icap_regs);
	icap_clean_bitstream_axlf(icap);
	ICAP_INFO(icap, "cleaned up successfully");
	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, icap);
	xocl_info(&pdev->dev, "Removed subdev %s\n", pdev->name);
	return 0;
}

static int xocl_icap_probe(struct platform_device *pdev)
{
	int ret;
	void **regs;
	const struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct xocl_icap *icap = devm_kzalloc(&pdev->dev, sizeof(struct xocl_icap), GFP_KERNEL);
	if (!icap)
		return -ENOMEM;
	icap->core.pdev =  pdev;
	platform_set_drvdata(pdev, icap);

	mutex_init(&icap->icap_lock);
	init_waitqueue_head(&icap->reader_wq);

	regs = (void **)&icap->icap_regs;
	if (res != NULL) {
		*regs = ioremap_nocache(res->start, res->end - res->start + 1);
		if (*regs == NULL) {
			ICAP_ERR(icap, "failed to map in register");
			ret = -EIO;
			goto failed;
		} else {
			ICAP_INFO(icap,
				"mapped in register @ 0x%p", *regs);
		}
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &icap_attr_group);
	if (ret) {
		ICAP_ERR(icap, "create icap attrs failed: %d", ret);
		goto failed;
	}

	if (ICAP_PRIVILEGED(icap)) {
#ifdef CONFIG_EFI
		if (efi_enabled(EFI_SECURE_BOOT)) {
			ICAP_INFO(icap, "secure boot mode detected");
			icap->sec_level = ICAP_SEC_SYSTEM;
		} else {
			icap->sec_level = ICAP_SEC_NONE;
		}
#else
		ICAP_INFO(icap, "no support for detection of secure boot mode");
		icap->sec_level = ICAP_SEC_NONE;
#endif
	}

	icap->cache_expire_secs = ICAP_DEFAULT_EXPIRE_SECS;

	icap_probe_chip(icap);
	ICAP_INFO(icap, "successfully initialized FPGA IDCODE 0x%x", icap->idcode);
	return 0;

failed:
	(void) xocl_icap_remove(pdev);
	return ret;

	xocl_info(&pdev->dev, "Probed subdev %s: resource %pr", pdev->name, res);
	return 0;
}

static const struct platform_device_id icap_id_table[] = {
	{ XOCL_ICAP, (kernel_ulong_t)&icap_ops },
	{ },
};


struct platform_driver xocl_icap_driver = {
	.driver	= {
		.name    = XOCL_ICAP,
	},
	.probe    = xocl_icap_probe,
	.remove   = xocl_icap_remove,
	.id_table = icap_id_table,
};
