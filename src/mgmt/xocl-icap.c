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
#include <linux/delay.h>

#include "xocl-icap.h"
#include "xocl-lib.h"
#include "xocl-features.h"
#include "xocl-mailbox-proto.h"
#include "xclbin.h"

static struct key *icap_keys = NULL;

static struct attribute_group icap_attr_group;

static int icap_parse_bitstream_axlf_section(struct platform_device *pdev,
	const struct axlf *xclbin, enum axlf_section_kind kind);
static void icap_set_data(struct xocl_icap *icap, struct xcl_pr_region *hwicap);
static uint64_t icap_get_data_nolock(struct platform_device *pdev, enum data_kind kind);
static uint64_t icap_get_data(struct platform_device *pdev, enum data_kind kind);
static const struct axlf_section_header *get_axlf_section_hdr(
	struct xocl_icap *icap, const struct axlf *top, enum axlf_section_kind kind);
static void icap_refresh_addrs(struct platform_device *pdev);

static void icap_iounmap_resources(struct xocl_icap *icap)
{
	int i = 0;
	if (icap->icap_state)
		iounmap(icap->icap_state);
	if (icap->icap_axi_gate)
		iounmap(icap->icap_axi_gate);
	for (i = 0; i < 3; i++) {
		if (!icap->icap_clock_bases[i])
			continue;
		iounmap(icap->icap_clock_bases[i]);
	}
}

static void *icap_ioremap_resource(const struct xocl_icap *icap, const char *name)
{
	void *io;
	const struct resource *res = xocl_subdev_resource(&icap->core, IORESOURCE_MEM, name);
	if (!res) {
		ICAP_ERR(icap, "Failed to find resource %s\n", name);
		return ERR_PTR(-ENXIO);
	}
	xocl_info(&icap->core.pdev->dev, "resource %pr", res);
	io = ioremap_nocache(res->start, resource_size(res));
	if (!io) {
		ICAP_ERR(icap, "Failed to map resource %s\n", name);
		return ERR_PTR(-EIO);
	}
	return io;
}

static int icap_ioremap_resources(struct xocl_icap *icap)
{
	int rc = 0;
	void *io = icap_ioremap_resource(icap, RESNAME_MEMCALIB);
	if (IS_ERR(io)) {
		rc = PTR_ERR(io);
		goto cleanup;
	}
	icap->icap_state = io;

	io = icap_ioremap_resource(icap, RESNAME_GATEPRPRP);
	if (IS_ERR(io)) {
		rc = PTR_ERR(io);
		goto cleanup;
	}
	icap->icap_axi_gate = io;

	io = icap_ioremap_resource(icap, RESNAME_CLKWIZKERNEL1);
	if (IS_ERR(io)) {
		rc = PTR_ERR(io);
		goto cleanup;
	}
	icap->icap_clock_bases[0] = io;

	io = icap_ioremap_resource(icap, RESNAME_CLKWIZKERNEL1);
	if (IS_ERR(io)) {
		rc = PTR_ERR(io);
		goto cleanup;
	}
	icap->icap_clock_bases[1] = io;

	io = icap_ioremap_resource(icap, RESNAME_CLKWIZKERNEL2);
	if (IS_ERR(io)) {
		rc = PTR_ERR(io);
		goto cleanup;
	}
	icap->icap_clock_bases[2] = io;

	io = icap_ioremap_resource(icap, RESNAME_CLKFREQ_K1_K2);
	if (IS_ERR(io)) {
		rc = PTR_ERR(io);
		goto cleanup;
	}
	icap->icap_clock_freq_counter = io;

	return 0;
cleanup:
	icap_iounmap_resources(icap);
	return rc;
}

unsigned short icap_get_ocl_frequency(const struct xocl_icap *icap, int idx)
{
#define XCL_INPUT_FREQ 100
	const u64 input = XCL_INPUT_FREQ;
	u32 val;
	u32 mul0, div0;
	u32 mul_frac0 = 0;
	u32 div1;
	u32 div_frac1 = 0;
	u64 freq = 0;
	char *base = NULL;

	if (ICAP_PRIVILEGED(icap)) {
		base = icap->icap_clock_bases[idx];
		if (!base)
			return 0;
		val = reg_rd(base + OCL_CLKWIZ_STATUS_OFFSET);
		if ((val & 1) == 0)
			return 0;

		val = reg_rd(base + OCL_CLKWIZ_CONFIG_OFFSET(0));

		div0 = val & 0xff;
		mul0 = (val & 0xff00) >> 8;
		if (val & BIT(26)) {
			mul_frac0 = val >> 16;
			mul_frac0 &= 0x3ff;
		}

		/*
		 * Multiply both numerator (mul0) and the denominator (div0) with 1000
		 * to account for fractional portion of multiplier
		 */
		mul0 *= 1000;
		mul0 += mul_frac0;
		div0 *= 1000;

		val = reg_rd(base + OCL_CLKWIZ_CONFIG_OFFSET(2));

		div1 = val & 0xff;
		if (val & BIT(18)) {
			div_frac1 = val >> 8;
			div_frac1 &= 0x3ff;
		}

		/*
		 * Multiply both numerator (mul0) and the denominator (div1) with 1000 to
		 * account for fractional portion of divider
		 */

		div1 *= 1000;
		div1 += div_frac1;
		div0 *= div1;
		mul0 *= 1000;
		if (div0 == 0) {
			ICAP_ERR(icap, "clockwiz 0 divider");
			return 0;
		}
		freq = (input * mul0) / div0;
	}
	return freq;
}

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

static void icap_get_ocl_frequency_max_min(struct xocl_icap *icap,
					   int idx, unsigned short *freq_max, unsigned short *freq_min)
{
	struct clock_freq_topology *topology = 0;
	int num_clocks = 0;

	if (!uuid_is_null(&icap->icap_bitstream_uuid)) {
		topology = icap->icap_clock_freq_topology;
		if (!topology)
			return;

		num_clocks = topology->m_count;

		if (idx >= num_clocks)
			return;

		if (freq_max)
			*freq_max = topology->m_clock_freq[idx].m_freq_Mhz;

		if (freq_min)
			*freq_min = frequency_table[0].ocl;
	}
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
	.ioctl = icap_ioctl,
	.fops = &icap_fops,
	.id = XOCL_SUBDEV_ICAP,
	.dnum = -1,
	.drv_post_init = xocl_post_init_icap,
	.drv_pre_exit = xocl_pre_exit_icap,
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

static ssize_t idcode_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;

	mutex_lock(&icap->icap_lock);
	cnt = sprintf(buf, "0x%x\n", icap->idcode);
	mutex_unlock(&icap->icap_lock);

	return cnt;
}

static DEVICE_ATTR_RO(idcode);

static unsigned int icap_get_clock_frequency_counter_khz(const struct xocl_icap *icap, int idx)
{
	u32 freq = 0, status;
	int times = 10;
	/*
	 * reset and wait until done
	 */

	if (ICAP_PRIVILEGED(icap)) {
		if (uuid_is_null(&icap->icap_bitstream_uuid))
			return freq;

		if (icap->icap_clock_freq_counter && idx < 2) {
			reg_wr(icap->icap_clock_freq_counter,
				OCL_CLKWIZ_STATUS_MEASURE_START);
			while (times != 0) {
				status = reg_rd(icap->icap_clock_freq_counter);
				if ((status & OCL_CLKWIZ_STATUS_MASK) ==
					OCL_CLKWIZ_STATUS_MEASURE_DONE)
					break;
				mdelay(1);
				times--;
			};
			if ((status & OCL_CLKWIZ_STATUS_MASK) ==
				OCL_CLKWIZ_STATUS_MEASURE_DONE)
				freq = reg_rd(icap->icap_clock_freq_counter + OCL_CLK_FREQ_COUNTER_OFFSET + idx*sizeof(u32));
			return freq;
		}

		if (icap->icap_clock_freq_counters[idx]) {
			reg_wr(icap->icap_clock_freq_counters[idx],
				OCL_CLKWIZ_STATUS_MEASURE_START);
			while (times != 0) {
				status =
				    reg_rd(icap->icap_clock_freq_counters[idx]);
				if ((status & OCL_CLKWIZ_STATUS_MASK) ==
					OCL_CLKWIZ_STATUS_MEASURE_DONE)
					break;
				mdelay(1);
				times--;
			};
			if ((status & OCL_CLKWIZ_STATUS_MASK) ==
				OCL_CLKWIZ_STATUS_MEASURE_DONE) {
				freq = (status & OCL_CLK_FREQ_V5_CLK0_ENABLED) ?
					reg_rd(icap->icap_clock_freq_counters[idx] + OCL_CLK_FREQ_V5_COUNTER_OFFSET) :
					reg_rd(icap->icap_clock_freq_counters[idx] + OCL_CLK_FREQ_COUNTER_OFFSET);
			}
		}
	}
	return freq;
}

static ssize_t clock_freqs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	int i, err;
	u32 freq_counter, freq, request_in_khz, tolerance;

	err = icap_xclbin_rd_lock(icap);
	if (err)
		return cnt;

	mutex_lock(&icap->icap_lock);
	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; i++) {
		freq = icap_get_ocl_frequency(icap, i);
		if (!uuid_is_null(&icap->icap_bitstream_uuid)) {
			freq_counter = icap_get_clock_frequency_counter_khz(icap, i);

			request_in_khz = freq*1000;
			tolerance = freq*50;

			if (abs(freq_counter-request_in_khz) > tolerance)
				ICAP_INFO(icap, "Frequency mismatch, Should be %u khz, Now is %ukhz", request_in_khz, freq_counter);
			cnt += sprintf(buf + cnt, "%d\n", DIV_ROUND_CLOSEST(freq_counter, 1000));
		} else
			cnt += sprintf(buf + cnt, "%d\n", freq);
	}

	mutex_unlock(&icap->icap_lock);
	icap_xclbin_rd_unlock(icap);
	return cnt;
}
static DEVICE_ATTR_RO(clock_freqs);

static ssize_t reader_cnt_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val = 0;

	mutex_lock(&icap->icap_lock);

	val = icap->reader_ref;

	mutex_unlock(&icap->icap_lock);

	return sprintf(buf, "%llu\n", val);
}
static DEVICE_ATTR_RO(reader_cnt);

static ssize_t cache_expire_secs_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val = 0;

	mutex_lock(&icap->icap_lock);
	if (!ICAP_PRIVILEGED(icap))
		val = icap->cache_expire_secs;

	mutex_unlock(&icap->icap_lock);
	return sprintf(buf, "%llu\n", val);
}

static ssize_t cache_expire_secs_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val;

	mutex_lock(&icap->icap_lock);
	if (kstrtou64(buf, 10, &val) == -EINVAL || val > 10) {
		xocl_err(&to_platform_device(dev)->dev,
			"usage: echo [0 ~ 10] > cache_expire_secs");
		return -EINVAL;
	}

	if (!ICAP_PRIVILEGED(icap))
		icap->cache_expire_secs = val;

	mutex_unlock(&icap->icap_lock);
	return count;
}
static DEVICE_ATTR_RW(cache_expire_secs);

static ssize_t clock_freqs_max_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	int i, err;
	unsigned short freq;

	err = icap_xclbin_rd_lock(icap);
	if (err)
		return cnt;

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; i++) {
		freq = 0;
		icap_get_ocl_frequency_max_min(icap, i, &freq, NULL);
		cnt += sprintf(buf + cnt, "%d\n", freq);
	}

	icap_xclbin_rd_unlock(icap);
	return cnt;
}
static DEVICE_ATTR_RO(clock_freqs_max);

static ssize_t clock_freqs_min_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct xocl_icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	int i, err;
	unsigned short freq;

	err = icap_xclbin_rd_lock(icap);
	if (err)
		return cnt;

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; i++) {
		freq = 0;
		icap_get_ocl_frequency_max_min(icap, i, NULL, &freq);
		cnt += sprintf(buf + cnt, "%d\n", freq);
	}

	icap_xclbin_rd_unlock(icap);
	return cnt;
}
static DEVICE_ATTR_RO(clock_freqs_min);

static struct attribute *icap_attrs[] = {
	&dev_attr_idcode.attr,
	&dev_attr_clock_freqs.attr,
	&dev_attr_reader_cnt.attr,
	&dev_attr_cache_expire_secs.attr,
	&dev_attr_clock_freqs_max.attr,
	&dev_attr_clock_freqs_min.attr,
	/*
	&dev_attr_sec_level.attr,
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
	icap_iounmap_resources(icap);
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
		xocl_info(&pdev->dev, "resource[0] %pr", res);
		*regs = ioremap_nocache(res->start, resource_size(res));
		if (*regs == NULL) {
			ICAP_ERR(icap, "failed to map in register");
			ret = -EIO;
			goto failed;
		}
	}

	icap_ioremap_resources(icap);

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
