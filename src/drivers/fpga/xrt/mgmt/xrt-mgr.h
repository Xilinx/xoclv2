/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Xilinx, Inc.
 *
 * Authors: Sonal.Santan@xilinx.com
 */

#ifndef _XMGMT_MGR_H_
#define _XMGMT_MGR_H_

#include <linux/fpga/fpga-mgr.h>

struct fpga_manager *xmgmt_fmgr_probe(struct xrt_device *xdev);
int xmgmt_fmgr_remove(struct fpga_manager *fmgr);

#endif
