/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 WITH Linux-syscall-note */
/*
 *  Copyright (C) 2015-2020, Xilinx Inc
 *
 */

/**
 * DOC: PCIe Kernel Driver for Managament Physical Function
 * Interfaces exposed by *xclmgmt* driver are defined in file, *mgmt-ioctl.h*.
 * Core functionality provided by *xmgmt* driver is described in the following table:
 *
 * =========== ============================== ==================================
 * Functionality           ioctl request code           data format
 * =========== ============================== ==================================
 * 1 FPGA image download   XCLMGMT_IOCICAPDOWNLOAD_AXLF xmgmt_ioc_bitstream_axlf
 * 2 CL frequency scaling  XCLMGMT_IOCFREQSCALE         xmgmt_ioc_freqscaling
 * =========== ============================== ==================================
 */

#ifndef _XMGMT_IOCALLS_POSIX_H_
#define _XMGMT_IOCALLS_POSIX_H_

#include <linux/ioctl.h>

#define XMGMT_IOC_MAGIC	'X'
#define XMGMT_NUM_SUPPORTED_CLOCKS 4

#define XMGMT_IOC_FREQ_SCALE 0x2
#define XMGMT_IOC_ICAP_DOWNLOAD_AXLF 0x6

/**
 * struct xmgmt_ioc_bitstream_axlf - load xclbin (AXLF) device image
 * used with XMGMT_IOCICAPDOWNLOAD_AXLF ioctl
 *
 * @xclbin:	Pointer to user's xclbin structure in memory
 */
struct xmgmt_ioc_bitstream_axlf {
	struct axlf *xclbin;
};

/**
 * struct xmgmt_ioc_freqscaling - scale frequencies on the board using Xilinx clock wizard
 * used with XMGMT_IOCFREQSCALE ioctl
 *
 * @ocl_region:	        PR region (currently only 0 is supported)
 * @ocl_target_freq:	Array of requested frequencies, a value o zero in the
 *                      array indicates leave untouched
 */
struct xmgmt_ioc_freqscaling {
	unsigned int ocl_region;
	unsigned short ocl_target_freq[XMGMT_NUM_SUPPORTED_CLOCKS];
};

#define DATA_CLK			0
#define KERNEL_CLK			1
#define SYSTEM_CLK			2

#define XMGMT_IOCICAPDOWNLOAD_AXLF				\
	_IOW(XMGMT_IOC_MAGIC, XMGMT_IOC_ICAP_DOWNLOAD_AXLF, struct xmgmt_ioc_bitstream_axlf)
#define XMGMT_IOCFREQSCALE					\
	_IOW(XMGMT_IOC_MAGIC, XMGMT_IOC_FREQ_SCALE, struct xmgmt_ioc_freqscaling)

/*
 * The following definitions are for binary compatibility with classic XRT management driver
 */

#define XCLMGMT_IOCICAPDOWNLOAD_AXLF XMGMT_IOCICAPDOWNLOAD_AXLF
#define XCLMGMT_IOCFREQSCALE XMGMT_IOCFREQSCALE

#define xclmgmt_ioc_bitstream_axlf xmgmt_ioc_bitstream_axlf
#define xclmgmt_ioc_freqscaling xmgmt_ioc_freqscaling

#endif
