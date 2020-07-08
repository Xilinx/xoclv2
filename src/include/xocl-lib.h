// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019, 2020 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#ifndef	_XOCL_CORE_LIBRARY_H_
#define	_XOCL_CORE_LIBRARY_H_

#include <linux/resource.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>

#include "xocl-features.h"

#define	ICAP_XCLBIN_V2		"xclbin2"
#define XOCL_AXLF_SIGNING_KEYS  ".xilinx_fpga_xclbin_keys"
#define	MGMTPF		         0
#define	USERPF		         1

#if PF == MGMTPF
#define SUBDEV_SUFFIX	        ".m"
#elif PF == USERPF
#define SUBDEV_SUFFIX	        ".u"
#endif

#define XOCL_FEATURE_ROM	"xocl-rom"
#define XOCL_IORES0		"iores0"
#define XOCL_IORES1		"iores1"
#define XOCL_IORES2		"iores2"
#define XOCL_XDMA		"dma.xdma"
#define XOCL_QDMA		"dma.qdma"
#define XOCL_MB_SCHEDULER	"mb_scheduler"
#define XOCL_XVC_PUB		"xvc_pub"
#define XOCL_XVC_PRI		"xvc_pri"
#define XOCL_NIFD_PRI		"nifd_pri"
#define XOCL_SYSMON		"xocl-sysmon"
#define XOCL_FIREWALL		"firewall"
#define	XOCL_MB			"microblaze"
#define	XOCL_PS			"processor_system"
#define	XOCL_XIIC		"xiic"
#define	XOCL_MAILBOX		"mailbox"
#define	XOCL_ICAP		"xocl-icap"
#define	XOCL_AXIGATE		"axigate"
#define	XOCL_MIG		"mig"
#define	XOCL_XMC		"xocl-xmc"
#define	XOCL_DNA		"dna"
#define	XOCL_FMGR		"fmgr"
#define	XOCL_FLASH		"flash"
#define XOCL_DMA_MSIX		"dma_msix"
#define	XOCL_MAILBOX_VERSAL	"mailbox_versal"
#define XOCL_ERT		"ert"
#define XOCL_REGION		"xocl-region"

#define XOCL_DEVNAME(str)	str SUBDEV_SUFFIX

enum xocl_subdev_id {
	XOCL_SUBDEV_FEATURE_ROM,
	XOCL_SUBDEV_AXIGATE,
	XOCL_SUBDEV_DMA,
	XOCL_SUBDEV_IORES,
	XOCL_SUBDEV_FLASH,
	XOCL_SUBDEV_MB_SCHEDULER,
	XOCL_SUBDEV_XVC_PUB,
	XOCL_SUBDEV_XVC_PRI,
	XOCL_SUBDEV_NIFD_PRI,
	XOCL_SUBDEV_SYSMON,
	XOCL_SUBDEV_AF,
	XOCL_SUBDEV_MIG,
	XOCL_SUBDEV_MB,
	XOCL_SUBDEV_PS,
	XOCL_SUBDEV_XIIC,
	XOCL_SUBDEV_MAILBOX,
	XOCL_SUBDEV_ICAP,
	XOCL_SUBDEV_DNA,
	XOCL_SUBDEV_FMGR,
	XOCL_SUBDEV_MIG_HBM,
	XOCL_SUBDEV_MAILBOX_VERSAL,
	XOCL_SUBDEV_OSPI_VERSAL,
	XOCL_SUBDEV_XMC,
	XOCL_SUBDEV_NUM
};

enum xocl_region_id {
	XOCL_REGION_STATIC,
	XOCL_REGION_BLD,
	XOCL_REGION_PRP,
	XOCL_REGION_URP,
	XOCL_REGION_LEGACYRP,
	XOCL_REGION_MAX,
};

#define XOCL_STATIC        "STATIC"
#define	XOCL_BLD           "BLD"
#define	XOCL_PRP           "PRP"
#define	XOCL_URP           "URP"
#define	XOCL_LEGACYR       "LEGACYPR"

#define	FLASH_TYPE_SPI	   "spi"
#define	FLASH_TYPE_QSPIPS  "qspi_ps"

#define XOCL_VSEC_UUID_ROM          0x50
#define XOCL_VSEC_FLASH_CONTROLER   0x51
#define XOCL_VSEC_PLATFORM_INFO     0x52
#define XOCL_VSEC_MAILBOX           0x53
#define XOCL_VSEC_PLAT_RECOVERY     0x00
#define XOCL_VSEC_PLAT_1RP          0x01
#define XOCL_VSEC_PLAT_2RP          0x02

#define XOCL_SUBDEV_MAX_INST	    64
#define XOCL_MAXNAMELEN	            64
#define XOCL_MAX_DEVICES	    16
#define MAX_M_COUNT      	    XOCL_SUBDEV_MAX_INST
#define XOCL_MAX_FDT_LEN	    1024 * 512
#define XOCL_EBUF_LEN               512

enum data_kind {
	MIG_CALIB,
	DIMM0_TEMP,
	DIMM1_TEMP,
	DIMM2_TEMP,
	DIMM3_TEMP,
	FPGA_TEMP,
	CLOCK_FREQ_0,
	CLOCK_FREQ_1,
	FREQ_COUNTER_0,
	FREQ_COUNTER_1,
	VOL_12V_PEX,
	VOL_12V_AUX,
	CUR_12V_PEX,
	CUR_12V_AUX,
	SE98_TEMP0,
	SE98_TEMP1,
	SE98_TEMP2,
	FAN_TEMP,
	FAN_RPM,
	VOL_3V3_PEX,
	VOL_3V3_AUX,
	VPP_BTM,
	VPP_TOP,
	VOL_5V5_SYS,
	VOL_1V2_TOP,
	VOL_1V2_BTM,
	VOL_1V8,
	VCC_0V9A,
	VOL_12V_SW,
	VTT_MGTA,
	VOL_VCC_INT,
	CUR_VCC_INT,
	IDCODE,
	IPLAYOUT_AXLF,
	MEMTOPO_AXLF,
	CONNECTIVITY_AXLF,
	DEBUG_IPLAYOUT_AXLF,
	PEER_CONN,
	XCLBIN_UUID,
	CLOCK_FREQ_2,
	CLOCK_FREQ_3,
	FREQ_COUNTER_2,
	FREQ_COUNTER_3,
	PEER_UUID,
	HBM_TEMP,
	CAGE_TEMP0,
	CAGE_TEMP1,
	CAGE_TEMP2,
	CAGE_TEMP3,
	VCC_0V85,
	SER_NUM,
	MAC_ADDR0,
	MAC_ADDR1,
	MAC_ADDR2,
	MAC_ADDR3,
	REVISION,
	CARD_NAME,
	BMC_VER,
	MAX_PWR,
	FAN_PRESENCE,
	CFG_MODE,
	VOL_VCC_3V3,
	CUR_3V3_PEX,
	CUR_VCC_0V85,
	VOL_HBM_1V2,
	VOL_VPP_2V5,
	VOL_VCCINT_BRAM,
	XMC_VER,
	EXP_BMC_VER,
	XMC_OEM_ID,
};

enum mb_kind {
	DAEMON_STATE,
	CHAN_STATE,
	CHAN_SWITCH,
	COMM_ID,
	VERSION,
};

#define RESNAME_ICAP            "icap"
#define RESNAME_MEMCALIB        "memcalib"
#define RESNAME_GATEPRPRP       "gateprp"
#define RESNAME_CLKWIZKERNEL1   "clkwizkernel1"
#define RESNAME_CLKWIZKERNEL2   "clkwizkernel2"
#define RESNAME_CLKWIZKERNEL3   "clkwizkernel3"
#define RESNAME_CLKFREQ_K1_K2   "clkfreq_k1_k2"
#define RESNAME_CLKFREQ_K1      "clkfreq_k1"
#define RESNAME_CLKFREQ_K2      "clkfreq_k2"

struct xocl_vsec_header {
	u32		format;
	u32		length;
	u32		entry_sz;
	u32		rsvd;
};

struct xmgmt_dev;
struct xocl_subdev_info;

/*
 * Populated by subdev drivers and is used by xocl core. It represents
 * the PLATFORM DEVICE.
 * This should be registered as driver_data in platform_device_id
 */
struct xocl_subdev_base {
	struct platform_device	*pdev;
	struct cdev              chr_dev;
	struct device           *sys_device;
};

/*
 * Subdev driver for a subdev. Provides services to clients and manages the subdev
 * It represents PLATFORM DRIVER.
 */
struct xocl_subdev_drv {
	/* Backends if defined for a subdev are called by xocl_subdev_ioctl/offline/online
	   exported functions defined below */
	long (*ioctl)(struct platform_device *pdev, unsigned int cmd, unsigned long arg);
	int (*offline)(struct platform_device *pdev);
	int (*online)(struct platform_device *pdev);
	/* Populate this if subdev defines its own file operations */
	const struct file_operations	*fops;
	/* If fops is defined then xocl will handle the mechanics of char device (un)registration */
	dev_t			         dnum;
	struct ida                       minor;
	enum xocl_subdev_id	         id;
	/* If defined these are called as part of driver (un)registration */
	int (*drv_post_init)(struct xocl_subdev_drv *ops);
	void (*drv_pre_exit)(struct xocl_subdev_drv *ops);
};

struct xocl_subdev_info {
	enum xocl_subdev_id	 id;
	const char	        *name;
	struct resource	        *res;
	int			 num_res;
	void		        *priv_data;
	int			 data_len;
	bool			 multi_inst;
	int			 level;
	char		        *bar_idx;
	int			 dyn_ip;
	const char	        *override_name;
	int			 override_idx;
};

/* TODO: Update this struct to add concept of regions which holds arrays of subdev_info */
struct xocl_board_private {
	uint64_t		flags;
	struct xocl_subdev_info	*subdev_info;
	uint32_t		subdev_num;
	uint32_t		dsa_ver;
	bool			xpr;
	char			*flash_type; /* used by xbflash */
	char			*board_name; /* used by xbflash */
	bool			mpsoc;
	uint64_t		p2p_bar_sz;
	const char		*vbnv;
	const char		*sched_bin;
};

/*
 * A region contains one or more subdevs.
 */
struct xocl_region {
	struct xmgmt_dev        *lro;
	enum xocl_region_id      id;
	struct platform_device  *region;
	int                      child_count;
	struct xocl_subdev_base *children[1];
};

struct xocl_from_core {
	struct FeatureRomHeader	header;
	bool			unified;
	bool			mb_mgmt_enabled;
	bool			mb_sche_enabled;
	bool			are_dev;
	bool			aws_dev;
	bool			runtime_clk_scale_en;
	char			uuid[65];
	bool			passthrough_virt_en;
};

struct xocl_dev_core {
	// TODO: Remove this PCIe device from here
	struct pci_dev		*pdev;
	struct mutex 		lock;
	struct fpga_manager    *mgr;
	u32			bar_idx;
	void __iomem		*bar_addr;
	resource_size_t		bar_size;
	resource_size_t		feature_rom_offset;

	u32			intr_bar_idx;
	void __iomem		*intr_bar_addr;
	resource_size_t		intr_bar_size;

	struct task_struct      *poll_thread;

	char			*fdt_blob;
	u32			fdt_blob_sz;
	struct xocl_board_private priv;

	rwlock_t		rwlock;
	struct xocl_from_core   from;
	char			ebuf[XOCL_EBUF_LEN + 1];
};

/*
 * Exported framework functions for use by subdev clients. These exported functions
 * call into private implementations of these (if defined) by the subdevs.
 * These complement "probe" and "remove" functions which are already handled by
 * platform driver model.
 */
long xocl_subdev_ioctl(struct xocl_subdev_base *subdev, unsigned int cmd,
		       unsigned long arg);
int xocl_subdev_offline(struct xocl_subdev_base *subdev);
int xocl_subdev_online(struct xocl_subdev_base *subdev);
const struct xocl_subdev_base *xocl_lookup_subdev(const struct xocl_region *region,
						  enum xocl_subdev_id key);
static inline const struct resource *xocl_subdev_resource(const struct xocl_subdev_base *subdev,
							  unsigned int type, const char *name)
{
	return platform_get_resource_byname(subdev->pdev, type, name);
}

static inline struct xocl_dev_core *xocl_get_xdev(const struct xocl_subdev_base *subdev)
{
	struct device *top = NULL;
	/* Go up to region */
	struct device *rdev = subdev->pdev->dev.parent;
	if (!rdev)
		return NULL;
	/* Now go up to xmgmt-drv */
	top = rdev->parent;
	if (!top)
		return NULL;
	return dev_get_drvdata(top);
}

static inline struct xocl_subdev_base *xocl_get_subdev(struct platform_device *pdev)
{
	return platform_get_drvdata(pdev);
}

static inline const char *xocl_subdev_name(const struct xocl_subdev_base *subdev)
{
	return subdev->pdev->name;
}

static inline const struct platform_device_id *subdev_get_device_id(const struct xocl_subdev_base *subdev)
{
	return platform_get_device_id(subdev->pdev);
}

int xocl_subdev_cdev_create(struct xocl_subdev_base *subdev);
int xocl_subdev_cdev_destroy(struct xocl_subdev_base *subdev);

static inline bool xocl_clk_scale_on(const struct xocl_dev_core *core) {
	return core->from.runtime_clk_scale_en;
}

static inline bool xocl_mb_mgmt_on(const struct xocl_dev_core *core) {
	return core->from.mb_mgmt_enabled;
}

static inline bool xocl_mb_sched_on(const struct xocl_dev_core *core) {
	return core->from.mb_sche_enabled;
}
#define xocl_err(dev, fmt, args...)					\
	dev_err(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xocl_warn(dev, fmt, args...)			\
	dev_warn(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xocl_info(dev, fmt, args...)			\
	dev_info(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xocl_dbg(dev, fmt, args...)			\
	dev_dbg(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)

#define	XOCL_PL_TO_PCI_DEV(pldev)		\
	to_pci_dev(pldev->dev.parent->parent)

#define	XOCL_READ_REG32(addr)		\
	ioread32(addr)
#define	XOCL_WRITE_REG32(val, addr)	\
	iowrite32(val, addr)

static inline void xocl_memcpy_fromio(void *buf, void *iomem, u32 size)
{
	int i;

	BUG_ON(size & 0x3);

	for (i = 0; i < size / 4; i++)
		((u32 *)buf)[i] = ioread32((char *)(iomem) + sizeof(u32) * i);
}

static inline void xocl_memcpy_toio(void *iomem, void *buf, u32 size)
{
	int i;

	BUG_ON(size & 0x3);

	for (i = 0; i < size / 4; i++)
		iowrite32(((u32 *)buf)[i], ((char *)(iomem) + sizeof(u32) * i));
}

#endif
