/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Xilinx, Inc.
 *
 * Authors:
 *	Cheng Zhen <maxz@xilinx.com>
 */

#ifndef	_XOCL_ROOT_H_
#define	_XOCL_ROOT_H_

int xroot_probe(struct pci_dev *pdev, void **root);
void xroot_remove(void *root);

#endif	/* _XOCL_ROOT_H_ */
