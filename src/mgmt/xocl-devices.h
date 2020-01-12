// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019, 2020 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#ifndef	_XOCL_DEVICES_H_
#define	_XOCL_DEVICES_H_

#include "xocl-lib.h"

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
	                .name   = RESNAME_ICAP,		\
			.start	= 0x020000,		\
			.end	= 0x020119,		\
			.flags  = IORESOURCE_MEM,	\
		},					\
		{					\
			.name	= RESNAME_MEMCALIB,	\
			.start	= 0x032000,		\
			.end	= 0x032003,		\
			.flags  = IORESOURCE_MEM,	\
		},					\
		{					\
			.name	= RESNAME_GATEPRPRP,	\
			.start	= 0x030000,		\
			.end	= 0x03000b,		\
			.flags  = IORESOURCE_MEM,	\
		},					\
		{					\
			.name	= RESNAME_CLKWIZKERNEL1,\
			.start	= 0x050000,		\
			.end	= 0x050fff,		\
			.flags  = IORESOURCE_MEM,	\
		},					\
		{					\
			.name	= RESNAME_CLKWIZKERNEL2,\
			.start	= 0x051000,		\
			.end	= 0x051fff,		\
			.flags  = IORESOURCE_MEM,	\
		},                                      \
		{	                                \
			.name	= RESNAME_CLKFREQ_K1_K2,\
			.start	= 0x052000,		\
			.end	= 0x052fff,		\
			.flags  = IORESOURCE_MEM,	\
		},			                \
	})

#define	XOCL_RES_XMC					\
		((struct resource []) {			\
			{				\
			.start	= 0x120000,		\
			.end 	= 0x121FFF,		\
			.flags  = IORESOURCE_MEM,	\
			},				\
			{				\
			.start	= 0x131000,		\
			.end 	= 0x131FFF,		\
			.flags  = IORESOURCE_MEM,	\
			},				\
			{				\
			.start	= 0x140000,		\
			.end 	= 0x15FFFF,		\
			.flags  = IORESOURCE_MEM,	\
			},				\
			{				\
			.start	= 0x160000,		\
			.end 	= 0x17FFFF,		\
			.flags  = IORESOURCE_MEM,	\
			},				\
			{				\
			.start	= 0x190000,		\
			.end 	= 0x19FFFF,		\
			.flags  = IORESOURCE_MEM,	\
			},				\
			/* RUNTIME CLOCK SCALING FEATURE BASE */	\
			{				\
			.start	= 0x053000,		\
			.end	= 0x053fff,		\
			.flags	= IORESOURCE_MEM,	\
			},				\
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
		(char []){ 0 },                         \
		0,                                      \
		NULL,                                   \
		0,                                      \
	}

#define	XOCL_DEVINFO_XMC_MGMT				\
	{						\
		XOCL_SUBDEV_MB,				\
		XOCL_XMC,				\
		XOCL_RES_XMC,				\
		ARRAY_SIZE(XOCL_RES_XMC),		\
		NULL,                                   \
		0,                                      \
		false,                                  \
		0,                                      \
		(char []){ 0 },                         \
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
		(char []){ 0 },                         \
		0,                                      \
		NULL,                                   \
		0,                                      \
	}

#define	MGMT_RES_XBB_DSA52						\
		((struct xocl_subdev_info []) {				\
			XOCL_DEVINFO_FEATURE_ROM,			\
			XOCL_DEVINFO_ICAP_MGMT,                         \
			XOCL_DEVINFO_XMC_MGMT,			        \
		})

#define	XOCL_BOARD_MGMT_XBB_DSA52					\
	(struct xocl_board_private){					\
		.flags		= 0,					\
		.subdev_info	= MGMT_RES_XBB_DSA52,			\
		.subdev_num     = ARRAY_SIZE(MGMT_RES_XBB_DSA52),	\
		.flash_type     = FLASH_TYPE_SPI,			\
	}

#endif
