/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XRT_ROOT_H_
#define	_XRT_ROOT_H_

#include <linux/pci.h>
#include "xrt-subdev.h"

int xroot_probe(struct pci_dev *pdev, void **root);
void xroot_remove(void *root);
bool xroot_wait_for_bringup(void *root);
int xroot_add_vsec_node(void *root, char *dtb);
int xroot_create_partition(void *root, char *dtb);
int xroot_add_simple_node(void *root, char *dtb, const char *endpoint);
void xroot_hot_reset(struct pci_dev *pdev);
void xroot_broadcast(void *root, enum xrt_events evt);

#endif	/* _XRT_ROOT_H_ */
