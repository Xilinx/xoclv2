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
#include "events.h"

struct xroot;

/*
 * Defines physical function (MPF / UPF) specific operations
 * needed in common root driver.
 */
struct xroot_pf_cb {
	void (*xpc_hot_reset)(struct pci_dev *pdev);
};

int xroot_probe(struct pci_dev *pdev, struct xroot_pf_cb *cb,
	struct xroot **root);
void xroot_remove(struct xroot *root);
bool xroot_wait_for_bringup(struct xroot *root);
int xroot_add_vsec_node(struct xroot *root, char *dtb);
int xroot_create_partition(struct xroot *xr, char *dtb);
int xroot_add_simple_node(struct xroot *root, char *dtb, const char *endpoint);
void xroot_broadcast(struct xroot *root, enum xrt_events evt);

#endif	/* _XRT_ROOT_H_ */
