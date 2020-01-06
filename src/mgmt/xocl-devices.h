// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019, 2020 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#ifndef	_XMGMT_ALVEO_DEVICES_H_
#define	_XMGMT_ALVEO_DEVICES_H_

#include <linux/resource.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#define	ICAP_XCLBIN_V2		"xclbin2"

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

#define XOCL_DEVNAME(str)	str SUBDEV_SUFFIX

enum subdev_id {
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
	XOCL_SUBDEV_NUM
};

enum region_id {
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

#define XOCL_MAXNAMELEN	            64
#define XOCL_MAX_DEVICES	    16

struct xocl_vsec_header {
	u32		format;
	u32		length;
	u32		entry_sz;
	u32		rsvd;
};

struct xmgmt_dev;
struct xocl_subdev_info;

/*
 * Populated by subdev drivers and is used by xocl core.
 * This should be registered as driver_data in platform_device_id
 */
struct xocl_subdev_ops {
	/* Called by xocl_subdev_ioctl/offline/online defined below */
	long (*ioctl)(struct platform_device *pdev, unsigned int cmd, unsigned long arg);
	int (*offline)(struct platform_device *pdev);
	int (*online)(struct platform_device *pdev);
	/* Populate this if subdev defines its own file operations */
	const struct file_operations	*fops;
	/* Set this to -1 if subdev intends to create a device node; xocl will handle the mechanics of
	   char device (un)registration */
	dev_t			         dev;
};

struct xocl_subdev_info {
	enum subdev_id		 id;
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

struct xocl_region {
	struct xmgmt_dev       *lro;
	enum region_id          id;
	struct platform_device *region;
	int                     child_count;
	struct platform_device *children[1];
};


#define	XOCL_RES_FEATURE_ROM				\
		((struct resource []) {			\
			{				\
			.start	= 0xB0000,		\
			.end	= 0xB0FFF,		\
			.flags	= IORESOURCE_MEM,	\
			}				\
		})

#define	XOCL_RES_ICAP_MGMT				\
	((struct resource []) {				\
		{					\
			.start	= 0x020000,		\
			.end	= 0x020119,		\
			.flags  = IORESOURCE_MEM,	\
		},					\
	})

#define	XOCL_RES_SYSMON					\
		((struct resource []) {			\
			{				\
			.start	= 0xA0000,		\
			.end 	= 0xAFFFF,		\
			.flags  = IORESOURCE_MEM,	\
			}				\
		})

#define	XOCL_DEVINFO_FEATURE_ROM			\
	{						\
		XOCL_SUBDEV_FEATURE_ROM,		\
		XOCL_FEATURE_ROM,			\
		XOCL_RES_FEATURE_ROM,			\
		ARRAY_SIZE(XOCL_RES_FEATURE_ROM),	\
		NULL,                                   \
		0,                                      \
		false,                                  \
		0,                                      \
		(char []){ 0 },                         \
		0,                                      \
		NULL,                                   \
		0,                                      \
	}


#define	XOCL_DEVINFO_ICAP_MGMT				\
	{						\
		XOCL_SUBDEV_ICAP,			\
		XOCL_ICAP,				\
		XOCL_RES_ICAP_MGMT,			\
		ARRAY_SIZE(XOCL_RES_ICAP_MGMT),		\
		NULL,                                   \
		0,                                      \
		false,                                  \
		0,                                      \
		(char []){ 2 },                         \
		0,                                      \
		NULL,                                   \
		0,                                      \
	}

#define	XOCL_DEVINFO_SYSMON				\
	{						\
		XOCL_SUBDEV_SYSMON,			\
		XOCL_SYSMON,				\
		XOCL_RES_SYSMON,			\
		ARRAY_SIZE(XOCL_RES_SYSMON),		\
		NULL,                                   \
		0,                                      \
		false,                                  \
		0,                                      \
		(char []){ 2 },                         \
		0,                                      \
		NULL,                                   \
		0,                                      \
	}

#define	MGMT_RES_XBB_DSA52						\
		((struct xocl_subdev_info []) {				\
			XOCL_DEVINFO_FEATURE_ROM,			\
			XOCL_DEVINFO_ICAP_MGMT, 			\
			XOCL_DEVINFO_SYSMON,	         	 	\
		})

#define	XOCL_BOARD_MGMT_XBB_DSA52					\
	(struct xocl_board_private){					\
		.flags		= 0,					\
		.subdev_info	= MGMT_RES_XBB_DSA52,			\
		.subdev_num     = ARRAY_SIZE(MGMT_RES_XBB_DSA52),	\
		.flash_type     = FLASH_TYPE_SPI,			\
	}

/*
 * Exported framework functions for use by subdev clients. These exported functions
 * call into private implementations of these (if defined) by the subdevs.
 * These complement "probe" and "remove" functions which are already handled by
 * platform driver model.
 */
long xocl_subdev_ioctl(struct platform_device *pdev, unsigned int cmd,
		       unsigned long arg);
int xocl_subdev_offline(struct platform_device *pdev);
int xocl_subdev_online(struct platform_device *pdev);

#define xocl_err(dev, fmt, args...)			\
	dev_err(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xocl_warn(dev, fmt, args...)			\
	dev_warn(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xocl_info(dev, fmt, args...)			\
	dev_info(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)
#define xocl_dbg(dev, fmt, args...)			\
	dev_dbg(dev, "dev %llx, %s: "fmt, (u64)dev, __func__, ##args)

#define	XOCL_PL_TO_PCI_DEV(pldev)		\
	to_pci_dev(pldev->dev.parent->parent)

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
