// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Xilinx, Inc. All rights reserved.
 *
 * Authors: sonal.santan@xilinx.com
 */

#ifndef	_XMGMT_ALVEO_DEVICES_H_
#define	_XMGMT_ALVEO_DEVICES_H_

#include <linux/resource.h>
#include <linux/platform_device.h>
//#include <linux/io.h>

#define	MGMTPF		0
#define	USERPF		1

#if PF == MGMTPF
#define SUBDEV_SUFFIX	".m"
#elif PF == USERPF
#define SUBDEV_SUFFIX	".u"
#endif

#define XOCL_FEATURE_ROM	"alveo-rom"
#define XOCL_IORES0		"iores0"
#define XOCL_IORES1		"iores1"
#define XOCL_IORES2		"iores2"
#define XOCL_XDMA		"dma.xdma"
#define XOCL_QDMA		"dma.qdma"
#define XOCL_MB_SCHEDULER	"mb_scheduler"
#define XOCL_XVC_PUB		"xvc_pub"
#define XOCL_XVC_PRI		"xvc_pri"
#define XOCL_NIFD_PRI		"nifd_pri"
#define XOCL_SYSMON		"sysmon"
#define XOCL_FIREWALL		"firewall"
#define	XOCL_MB			"microblaze"
#define	XOCL_PS			"processor_system"
#define	XOCL_XIIC		"xiic"
#define	XOCL_MAILBOX		"mailbox"
#define	XOCL_ICAP		"alveo-icap"
#define	XOCL_AXIGATE		"axigate"
#define	XOCL_MIG		"mig"
#define	XOCL_XMC		"xmc"
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

struct xmgmt_dev;
struct xocl_subdev_info;

struct xmgmt_subdev_ops {
	int (*init)(struct platform_device *pdev, const struct xocl_subdev_info *detail);
	void (*uinit)(struct platform_device *pdev);
	long (*ioctl)(struct platform_device *pdev, unsigned int cmd, unsigned long arg);
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
	struct xmgmt_subdev_ops *ops;
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

struct xmgmt_region {
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


#define	XOCL_DEVINFO_FEATURE_ROM			\
	{						\
		XOCL_SUBDEV_FEATURE_ROM,		\
		XOCL_FEATURE_ROM,			\
		XOCL_RES_FEATURE_ROM,			\
		ARRAY_SIZE(XOCL_RES_FEATURE_ROM),	\
	}


#define	XOCL_DEVINFO_ICAP_MGMT				\
	{						\
		XOCL_SUBDEV_ICAP,			\
		XOCL_ICAP,				\
		XOCL_RES_ICAP_MGMT,			\
		ARRAY_SIZE(XOCL_RES_ICAP_MGMT),		\
	}

#define	XOCL_RES_SYSMON					\
		((struct resource []) {			\
			{				\
			.start	= 0xA0000,		\
			.end 	= 0xAFFFF,		\
			.flags  = IORESOURCE_MEM,	\
			}				\
		})

#define	XOCL_DEVINFO_SYSMON				\
	{						\
		XOCL_SUBDEV_SYSMON,			\
		XOCL_SYSMON,				\
		XOCL_RES_SYSMON,			\
		ARRAY_SIZE(XOCL_RES_SYSMON),		\
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

#endif
