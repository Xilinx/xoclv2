// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Alveo Management Function Driver
 *
 * Copyright (C) 2019 Xilinx, Inc.
 *
 * Authors: sonal.santan@xilinx.com
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/aer.h>
#include <linux/platform_device.h>

#include "xmgmt-drv.h"
#include "xocl-devices.h"

static const struct pci_device_id pci_ids[] = {
        { PCI_DEVICE(0x10EE, 0x5000), },
        { PCI_DEVICE(0x10EE, 0x5010), },
	{ 0, }
};

static dev_t xmgmt_devnode;
static struct class *xmgmt_class;

static const struct xocl_board_private u200 = XOCL_BOARD_MGMT_XBB_DSA52;

int xmgmt_config_pci(struct xmgmt_dev *lro)
{
	struct pci_dev *pdev = lro->pdev;
	int rc;

	rc = pci_enable_device(pdev);
	if (rc) {
		xmgmt_err(&pdev->dev, "pci_enable_device() failed, rc = %d.\n",
			rc);
		goto failed;
	}

	pci_set_master(pdev);

	rc = pcie_get_readrq(pdev);
	if (rc < 0) {
		xmgmt_err(&pdev->dev, "failed to read mrrs %d\n", rc);
		goto failed;
	}
	if (rc > 512) {
		rc = pcie_set_readrq(pdev, 512);
		if (rc) {
			xmgmt_err(&pdev->dev, "failed to force mrrs %d\n", rc);
			goto failed;
		}
	}
	rc = 0;

failed:
	return rc;
}

/*
 * create_char() -- create a character device interface to data or control bus
 *
 * If at least one SG DMA engine is specified, the character device interface
 * is coupled to the SG DMA file operations which operate on the data bus. If
 * no engines are specified, the interface is coupled with the control bus.
 */
static int create_char(struct xmgmt_dev *lro)
{
        int rc;
	struct xmgmt_char *lro_char = &lro->user_char_dev;

	/* couple the control device file operations to the character device */
	lro_char->cdev = cdev_alloc();
	if (!lro_char->cdev)
		return -ENOMEM;

//	lro_char->cdev->ops = &ctrl_fops;
	lro_char->cdev->owner = THIS_MODULE;
	lro_char->cdev->dev = MKDEV(MAJOR(xmgmt_devnode), lro->dev_minor);
	rc = cdev_add(lro_char->cdev, lro_char->cdev->dev, 1);
	if (rc < 0) {
		memset(lro_char, 0, sizeof(*lro_char));
		printk(KERN_INFO "cdev_add() = %d\n", rc);
		goto fail_add;
	}

	lro_char->sys_device = device_create(xmgmt_class,
				&lro->pdev->dev,
				lro_char->cdev->dev, NULL,
				XMGMT_MODULE_NAME "%d", lro->instance);

	if (IS_ERR(lro_char->sys_device)) {
		rc = PTR_ERR(lro_char->sys_device);
		goto fail_device;
	}

	return 0;

fail_device:
	cdev_del(lro_char->cdev);
fail_add:
	return rc;
}

static int destroy_char(struct xmgmt_char *lro_char)
{
	BUG_ON(!lro_char);
	BUG_ON(!xmgmt_class);

	if (lro_char->sys_device)
		device_destroy(xmgmt_class, lro_char->cdev->dev);
	cdev_del(lro_char->cdev);

	return 0;
}

/*
 * Compute the IP IOMEM resource absolute PCIe address based on PCIe BAR
 */
static void rebase_resources(struct pci_dev *pci_dev, struct platform_device *pdev,
			     const struct xocl_subdev_info *info)
{
	struct resource *res;
	int i;

	const resource_size_t iostart = pci_resource_start(pci_dev, (int)info->bar_idx[0]);
	for (i = 0; i < info->num_res; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			continue;
		res->start += iostart;
		res->end += iostart;
	}
}

static void xmgmt_subdevs_remove(struct xocl_region *part)
{
	int i;
	struct device *dev = &part->lro->pdev->dev;
	for (i = 0; i < u200.subdev_num; i++) {
		if (!part->children[i])
			continue;
		xmgmt_info(dev, "Remove child subdev[%d] %s: 0x%px.0x%px\n", i, part->children[i]->name, part, part->children[i]);
		/* Only unregister, no put as the former does release the reference */
		platform_device_unregister(part->children[i]);
		part->children[i] = NULL;
	}
}

static struct platform_device *xmgmt_subdev_probe(struct xocl_region *part,
						  struct xocl_subdev_info *info)
{
	/* WIP Start with U200 static region */
	int rc = 0;
	struct device *dev = &part->lro->pdev->dev;
	struct platform_device *pdev = platform_device_alloc(info->name, PLATFORM_DEVID_AUTO);
	xmgmt_info(dev, "Subdev 0x%px %s\n", pdev, info->name);
	if (!pdev)
		return ERR_PTR(-ENOMEM);;

	pdev->dev.parent = &part->region->dev;
	rc = platform_device_add_resources(pdev, info->res, info->num_res);
	if (rc)
		goto out_dev_put;
	rc = platform_device_add_data(pdev, &part->lro->core, sizeof(part->lro->core));
	if (rc)
		goto out_dev_put;
	rebase_resources(part->lro->pdev, pdev, info);
	rc = platform_device_add(pdev);
	if (rc)
		goto out_dev_put;
	return pdev;

out_dev_put:
	platform_device_put(pdev);
	return ERR_PTR(rc);
}

/*
 * Go through the DT and create platform drivers for each of the IP in this region
 * For now we assume all subdevs in xocl_board_private is for STATIC
 */
static int xmgmt_subdevs_probe(struct xocl_region *part)
{
	struct platform_device *child;
	int rc = 0;
	int i = 0;
	struct device *dev = &part->lro->pdev->dev;

	/* WIP Start with U200 static region for now */
	if (part->id != XOCL_REGION_STATIC)
		return 0;

	while (i < u200.subdev_num) {
		child = xmgmt_subdev_probe(part, &u200.subdev_info[i]);
		if (IS_ERR(child)) {
			rc = PTR_ERR(child);
			goto out_free;
		}
		xmgmt_info(dev, "Add child subdev[%d] %s: 0x%px.0x%px\n", i,  child->name, part, child);
		part->children[i++] = child;
	}
	return 0;

out_free:
	xmgmt_subdevs_remove(part);
	return rc;
}

static inline size_t sizeof_xocl_region(const struct xocl_region *part)
{
	return offsetof(struct xocl_region, children) +
		sizeof(struct platform_device *) * part->child_count;
}

static void xmgmt_subdev_test(const struct xocl_region *part)
{
	int i = 0;
	struct device *dev = &part->lro->pdev->dev;

	/* WIP Start with U200 static region for now */
	if (part->id != XOCL_REGION_STATIC)
		return;

	while (i < u200.subdev_num) {
		xmgmt_info(dev, "Subdev[%d] 0x%px.0x%px test", i, part, part->children[i]);
		xocl_subdev_ioctl(part->children[i++], 0, 0);
	}
}

static struct xocl_region *xmgmt_part_probe(struct xmgmt_dev *lro, enum region_id id)
{
	int rc = -ENOMEM;
	// TODO: obtain the count of children IPs in this region in DT using id as key
	int child_count = u200.subdev_num;
	struct xocl_region *part = vzalloc(offsetof(struct xocl_region, children) +
					      sizeof(struct platform_device *) * child_count);
	if (part == NULL)
		return ERR_PTR(rc);

	part->child_count = child_count;
	part->lro = lro;
	part->id = id;
	part->region = platform_device_alloc("xocl-region", PLATFORM_DEVID_AUTO);
	if (!part->region)
		goto out_free;

	part->region->dev.parent = &lro->pdev->dev;
	rc = platform_device_add_data(part->region, part, sizeof_xocl_region(part));
	if (rc)
		goto out_dev_put;
	rc = platform_device_add(part->region);
	if (rc)
		goto out_dev_put;

	// TODO: Pass the DT information down for this region
	rc = xmgmt_subdevs_probe(part);
	if (rc)
		goto out_dev_unregister;

	return part;
out_dev_put:
	platform_device_put(part->region);
	part->region = NULL;
out_dev_unregister:
	if (part->region) platform_device_unregister(part->region);
out_free:
	vfree(part);
	return ERR_PTR(rc);
}

/*
 * Cleanup the regions after their children have been destroyed
 */
static void xmgmt_parts_remove(struct xmgmt_dev *lro)
{
	int i = lro->part_count;
	while (i > 0) {
		if (!lro->part[--i])
			continue;
		/* First takedown all the child IPs of this region */
		xmgmt_subdevs_remove(lro->part[i]);
		/* Now takedown this region */
		/* Only unregister, no put as the former does release the reference */
		platform_device_unregister(lro->part[i]->region);
		vfree(lro->part[i]);
		lro->part[i] = NULL;
	}
}

/*
 * The core of this function should be data driven using something like DT
 * Go through each region (static and dynamic) and create the subdevices
 * for the IPs present in the region.
 */
static int xmgmt_parts_probe(struct xmgmt_dev *lro)
{
	int rc = 0;
	struct xocl_region *part = NULL;
	part = xmgmt_part_probe(lro, XOCL_REGION_STATIC);
	if (IS_ERR(part))
		return PTR_ERR(part);
	xmgmt_info(&lro->pdev->dev, "Store part[0] 0x%px.0x%px \n", part, part->region);
	lro->part[0] = part;
	part = xmgmt_part_probe(lro, XOCL_REGION_LEGACYRP);
	if (IS_ERR(part)) {
		rc = PTR_ERR(part);
		goto out_free;
	}
	xmgmt_info(&lro->pdev->dev, "Store part[1] 0x%px.0x%px\n", part, part->region);
	lro->part[1] = part;
	return 0;
out_free:
	xmgmt_parts_remove(lro);
	return rc;
}

static int xmgmt_fmgr_probe(struct xmgmt_dev *lro)
{
	int rc = -ENOMEM;

	lro->fmgr = platform_device_alloc("xocl-fmgr", PLATFORM_DEVID_AUTO);
	xmgmt_info(&lro->pdev->dev, "FPGA Manager 0x%px\n", lro->fmgr);
	if (!lro->fmgr)
		return rc;

	lro->fmgr->dev.parent = &lro->pdev->dev;
	rc = platform_device_add_data(lro->fmgr, NULL, 0);
	if (rc)
		goto out_dev_put;
	rc = platform_device_add(lro->fmgr);
	if (rc)
		goto out_dev_put;
	return 0;

out_dev_put:
	platform_device_put(lro->fmgr);
	lro->fmgr = NULL;
	return rc;
}

/*
 * Device initialization is done in two phases:
 * 1. Minimum initialization - init to the point where open/close/mmap entry
 * points are working, sysfs entries work without register access, ioctl entry
 * point is completely disabled.
 * 2. Full initialization - driver is ready for use.
 * Once we pass minimum initialization point, probe function shall not fail.
 */
static int xmgmt_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rc = 0;
	struct xmgmt_dev *lro = NULL;
	int part_count = 0;

	xmgmt_info(&pdev->dev, "Driver: %s", XMGMT_DRIVER_VERSION);
	xmgmt_info(&pdev->dev, "probe(pdev = 0x%px, pci_id = 0x%px)\n", pdev, id);

	/* Assuming U200 XDMA legacy platform with two regions */
	/* allocate zeroed device book keeping structure */
	part_count = 2;
	lro = xmgmt_drvinst_alloc(&pdev->dev, offsetof(struct xmgmt_dev, part) +
				  sizeof(struct xocl_region *) * part_count);
	if (!lro) {
		xmgmt_err(&pdev->dev, "Could not kzalloc(xmgmt_dev).\n");
		rc = -ENOMEM;
		goto err_alloc;
	}
	lro->part_count = 2;

	/* create a device to driver reference */
	dev_set_drvdata(&pdev->dev, lro);
	/* create a driver to device reference */
	lro->pdev = pdev;
	lro->ready = false;

	rc = xmgmt_config_pci(lro);
	if (rc)
		goto err_alloc_minor;

	lro->instance = XMGMT_DEV_ID(pdev);
	rc = create_char(lro);
	if (rc) {
		xmgmt_err(&pdev->dev, "create_char(user_char_dev) failed\n");
		goto err_cdev;
	}

	rc = xmgmt_fmgr_probe(lro);
	if (rc)
		goto err_fmgr;

	rc = xmgmt_parts_probe(lro);
	if (rc)
		goto err_region;

	/*
	 * Now complete fpga mgr initialization. It needs access to STATIC or BLD in order
	 * to orchestrate download with ICAP, CW, AXI_GATE, etc.
	 */
	rc = platform_device_add_data(lro->fmgr, lro->part[0], sizeof_xocl_region(lro->part[0]));
	if (rc)
		goto err_fmgr_data;

	xmgmt_subdev_test(lro->part[0]);
	xmgmt_subdev_test(lro->part[1]);

	return 0;

#if 0
	xmgmt_drvinst_set_filedev(lro, lro->user_char_dev.cdev);

	mutex_init(&lro->busy_mutex);

	mgmt_init_sysfs(&pdev->dev);

	/* Probe will not fail from now on. */
	xmgmt_info(&pdev->dev, "minimum initialization done\n");

	/* No further initialization for MFG board. */
	if (minimum_initialization)
		return 0;

	xmgmt_extended_probe(lro);
#endif
err_fmgr_data:
	xmgmt_parts_remove(lro);
err_region:
	platform_device_put(lro->fmgr);
	platform_device_unregister(lro->fmgr);
err_fmgr:
	destroy_char(&lro->user_char_dev);
err_cdev:
err_alloc_minor:
	xmgmt_drvinst_free(lro);
err_alloc:
	pci_disable_device(pdev);

	return rc;
}

static void xmgmt_remove(struct pci_dev *pdev)
{
	struct xmgmt_dev *lro;

	if ((pdev == 0) || (dev_get_drvdata(&pdev->dev) == 0))
		return;


	lro = (struct xmgmt_dev *)dev_get_drvdata(&pdev->dev);
	xmgmt_info(&pdev->dev, "remove(0x%px) where pdev->dev.driver_data = 0x%px",
                 pdev, lro);

	BUG_ON(lro->pdev != pdev);
	xmgmt_parts_remove(lro);
	platform_device_unregister(lro->fmgr);
	destroy_char(&lro->user_char_dev);
	xmgmt_drvinst_free(lro);
	pci_disable_device(pdev);

#if 0
	xmgmt_connect_notify(lro, false);

	if (xmgmt_passthrough_virtualization_on(lro) &&
		!iommu_present(&pci_bus_type))
		pci_write_config_byte(pdev, 0x188, 0x0);

	xmgmt_thread_stop(lro);

	mgmt_fini_sysfs(&pdev->dev);

	xmgmt_subdev_destroy_all(lro);
	xmgmt_subdev_fini(lro);

	/* remove user character device */
	destroy_char(&lro->user_char_dev);

	/* unmap the BARs */
	unmap_bars(lro);

	pci_disable_device(pdev);

	xmgmt_free_dev_minor(lro);

	if (lro->core.fdt_blob)
		vfree(lro->core.fdt_blob);
	if (lro->core.dyn_subdev_store)
		vfree(lro->core.dyn_subdev_store);
	if (lro->userpf_blob)
		vfree(lro->userpf_blob);
	if (lro->bld_blob)
		vfree(lro->bld_blob);

	dev_set_drvdata(&pdev->dev, NULL);

	xmgmt_drvinst_free(lro);
#endif
}

static pci_ers_result_t mgmt_pci_error_detected(struct pci_dev *pdev,
	pci_channel_state_t state)
{
	switch (state) {
	case pci_channel_io_normal:
		xmgmt_info(&pdev->dev, "PCI normal state error\n");
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		xmgmt_info(&pdev->dev, "PCI frozen state error\n");
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		xmgmt_info(&pdev->dev, "PCI failure state error\n");
		return PCI_ERS_RESULT_DISCONNECT;
	default:
		xmgmt_info(&pdev->dev, "PCI unknown state %d error\n", state);
		break;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}

static const struct pci_error_handlers xmgmt_err_handler = {
	.error_detected = mgmt_pci_error_detected,
};

static struct pci_driver xmgmt_driver = {
	.name = XMGMT_MODULE_NAME,
	.id_table = pci_ids,
	.probe = xmgmt_probe,
	.remove = xmgmt_remove,
	/* resume, suspend are optional */
	.err_handler = &xmgmt_err_handler,
};

static int __init xmgmt_init(void)
{
	int res;

	pr_info(XMGMT_MODULE_NAME " init()\n");
	xmgmt_class = class_create(THIS_MODULE, "xmgmt_mgmt");
	if (IS_ERR(xmgmt_class))
		return PTR_ERR(xmgmt_class);

	res = alloc_chrdev_region(&xmgmt_devnode, 0,
				  XMGMT_MAX_DEVICES, XMGMT_MODULE_NAME);
	if (res)
		goto alloc_err;

	res = pci_register_driver(&xmgmt_driver);
	if (res)
		goto reg_err;

	return 0;

reg_err:
	unregister_chrdev_region(xmgmt_devnode, XMGMT_MAX_DEVICES);
alloc_err:
	pr_info(XMGMT_MODULE_NAME " init() err\n");
	class_destroy(xmgmt_class);
	return res;
}

static void xmgmt_exit(void)
{
	pr_info(XMGMT_MODULE_NAME" exit()\n");
	pci_unregister_driver(&xmgmt_driver);

	/* unregister this driver from the PCI bus driver */
	unregister_chrdev_region(xmgmt_devnode, XMGMT_MAX_DEVICES);
	class_destroy(xmgmt_class);
}

module_init(xmgmt_init);
module_exit(xmgmt_exit);

MODULE_DEVICE_TABLE(pci, pci_ids);
MODULE_VERSION(XMGMT_DRIVER_VERSION);
MODULE_AUTHOR("XRT Team <runtime@xilinx.com>");
MODULE_DESCRIPTION("Xilinx Alveo management function driver");
MODULE_LICENSE("GPL v2");
