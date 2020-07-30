/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 *  Copyright (C) 2015-2020, Xilinx Inc
 *
 */

/**
 * DOC: PCIe Kernel Driver for Managament Physical Function
 * Interfaces exposed by *xclmgmt* driver are defined in file, *mgmt-ioctl.h*.
 * Core functionality provided by *xclmgmt* driver is described in the following table:
 *
 * ==== ====================================== ============================== ==================================
 * #    Functionality                          ioctl request code             data format
 * ==== ====================================== ============================== ==================================
 * 1    FPGA image download                    XCLMGMT_IOCICAPDOWNLOAD_AXLF   xclmgmt_ioc_bitstream_axlf
 * 2    CL frequency scaling                   XCLMGMT_IOCFREQSCALE           xclmgmt_ioc_freqscaling
 * 3    PCIe hot reset                         XCLMGMT_IOCHOTRESET            NA
 * 4    CL reset                               XCLMGMT_IOCOCLRESET            NA
 * 5    Live boot FPGA from PROM               XCLMGMT_IOCREBOOT              NA
 * 6    Device sensors (current, voltage and   NA                             *hwmon* (xclmgmt_microblaze and
 *      temperature)                                                          xclmgmt_sysmon) interface on sysfs
 * 7    Querying device errors                 XCLMGMT_IOCERRINFO             xclErrorStatus
 * ==== ====================================== ============================== ==================================
 *
 */

#ifndef _XMGMT_IOCALLS_POSIX_H_
#define _XMGMT_IOCALLS_POSIX_H_

#include <linux/ioctl.h>

#define XCLMGMT_IOC_MAGIC	'X'
#define XCLMGMT_NUM_SUPPORTED_CLOCKS 4
#define XCLMGMT_NUM_ACTUAL_CLOCKS 2
#define XCLMGMT_NUM_FIREWALL_IPS 3
#define AWS_SHELL14             69605400

#define AXI_FIREWALL

enum XCLMGMT_IOC_TYPES {
	XCLMGMT_IOC_INFO,
	XCLMGMT_IOC_ICAP_DOWNLOAD,
	XCLMGMT_IOC_FREQ_SCALE,
	XCLMGMT_IOC_REBOOT = 5,
	XCLMGMT_IOC_ICAP_DOWNLOAD_AXLF,
	XCLMGMT_IOC_ERR_INFO,
	XCLMGMT_IOC_SW_MAILBOX,
	XCLMGMT_IOC_MAX
};


/**
 * struct xclmgmt_ioc_bitstream_axlf - load xclbin (AXLF) device image
 * used with XCLMGMT_IOCICAPDOWNLOAD_AXLF ioctl
 *
 * @xclbin:	Pointer to user's xclbin structure in memory
 */
struct xclmgmt_ioc_bitstream_axlf {
	struct axlf *xclbin;
};

/**
 * struct xclmgmt_ioc_freqscaling - scale frequencies on the board using Xilinx clock wizard
 * used with XCLMGMT_IOCFREQSCALE ioctl
 *
 * @ocl_region:	        PR region (currently only 0 is supported)
 * @ocl_target_freq:	Array of requested frequencies, a value o zero in the array indicates leave untouched
 */
struct xclmgmt_ioc_freqscaling {
	unsigned int ocl_region;
	unsigned short ocl_target_freq[XCLMGMT_NUM_SUPPORTED_CLOCKS];
};

#define DATA_CLK			0
#define KERNEL_CLK			1
#define SYSTEM_CLK			2

#define XCLMGMT_IOCICAPDOWNLOAD_AXLF	_IOW(XCLMGMT_IOC_MAGIC, XCLMGMT_IOC_ICAP_DOWNLOAD_AXLF, struct xclmgmt_ioc_bitstream_axlf)
#define XCLMGMT_IOCFREQSCALE		_IOW(XCLMGMT_IOC_MAGIC, XCLMGMT_IOC_FREQ_SCALE, struct xclmgmt_ioc_freqscaling)

#endif
