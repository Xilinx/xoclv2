// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019-2020 Xilinx, Inc.
 * Bulk of the code borrowed from XRT mgmt driver file, fmgr.c
 *
 * Authors: Sonal.Santan@xilinx.com
 */

#ifndef	_XMGMT_FMGR_H_
#define	_XMGMT_FMGR_H_

#include <linux/fpga/fpga-mgr.h>
#include <linux/mutex.h>

#include "xclbin.h"

/*
 * Container to capture and cache full xclbin as it is passed in blocks by FPGA
 * Manager. xocl needs access to full xclbin to walk through xclbin sections. FPGA
 * Manager's .write() backend sends incremental blocks without any knowledge of
 * xclbin format forcing us to collect the blocks and stitch them together here.
 * TODO:
 * 1. Add a variant of API, icap_download_bitstream_axlf() which works off kernel buffer
 * 2. Call this new API from FPGA Manager's write complete hook, xmgmt_pr_write_complete()
 */

enum xfpga_sec_level {
	XFPGA_SEC_NONE = 0,
	XFPGA_SEC_DEDICATE,
	XFPGA_SEC_SYSTEM,
	XFPGA_SEC_MAX = XFPGA_SEC_SYSTEM,
};

struct xfpga_klass {
	/* Refers to the static shell/BLD which contains the IPs necessary for reprogramming */
	struct xmgmt_region *fixed;
	struct axlf         *blob;
	char                 name[64];
	size_t               count;
	struct mutex         axlf_lock;
	int                  reader_ref;
	enum fpga_mgr_states state;
	enum xfpga_sec_level sec_level;
};

int xfpga_xclbin_download(struct fpga_manager *mgr);

#endif
