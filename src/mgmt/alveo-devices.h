// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Xilinx, Inc. All rights reserved.
 *
 * Authors: Sonal.Santan@Xilinx.com
 */

#ifndef	_XMGMT_ALVEO_DEVICES_H_
#define	_XMGMT_ALVEO_DEVICES_H_

#if PF == MGMTPF
#define SUBDEV_SUFFIX	".m"
#elif PF == USERPF
#define SUBDEV_SUFFIX	".u"
#endif

#define XOCL_FEATURE_ROM	"rom"
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
#define	XOCL_ICAP		"icap"
#define	XOCL_AXIGATE		"axigate"
#define	XOCL_MIG		"mig"
#define	XOCL_XMC		"xmc"
#define	XOCL_DNA		"dna"
#define	XOCL_FMGR		"fmgr"
#define	XOCL_FLASH		"flash"
#define XOCL_DMA_MSIX		"dma_msix"
#define	XOCL_MAILBOX_VERSAL	"mailbox_versal"
#define XOCL_ERT		"ert"

#define XRT_DEVNAME(str)	str SUBDEV_SUFFIX

#endif
