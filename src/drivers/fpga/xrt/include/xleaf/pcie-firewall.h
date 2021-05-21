/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors:
 *      Lizhi Hou <Lizhi.Hou@xilinx.com>
 */

#ifndef _XRT_PCIE_FIREWALL_H_
#define _XRT_PCIE_FIREWALL_H_

#include "xleaf.h"

/*
 * pcie firewall driver leaf calls.
 */
enum xrt_pcie_firewall_cmd {
	XRT_PFW_UNBLOCK = XRT_XLEAF_CUSTOM_BASE, /* See comments in xleaf.h */
};

struct xrt_pcie_firewall_unblock {
	u32	pf_index;
	u32	bar_index;
};

#endif  /* _XRT_PCIE_FIREWALL_H_ */
