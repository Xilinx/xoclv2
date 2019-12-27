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

#include "alveo-drv.h"
#include "alveo-devices.h"

static const struct pci_device_id pci_ids[] = {
        { PCI_DEVICE(0x10EE, 0x5000), },
	{ 0, }
};

static dev_t xmgmt_devnode;
static struct class *xmgmt_class;

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

static int xmgmt_subdev_probe(struct xmgmt_region *part)
{
	return 0;
}


static struct xmgmt_region *xmgmt_region_probe(struct xmgmt_dev *lro, enum region_id id)
{
	int rc = -ENOMEM;
	int child_count = 1; // obtain the count of children IPs in this region in DT using id as key
	struct xmgmt_region *part = vmalloc(offsetof(struct xmgmt_region, children) +
					      sizeof(struct platform_device *) * child_count);
	if (part == NULL)
		return ERR_PTR(-ENOMEM);

	part->lro = lro;
	part->id = id;
	part->region = platform_device_alloc("alveo-region", PLATFORM_DEVID_AUTO);
	xmgmt_info(&lro->pdev->dev, "Region 0x%p\n", part->region);
	if (!part->region)
		goto out_free;

	rc = platform_device_add_data(part->region, part, sizeof(*part));
	xmgmt_info(&lro->pdev->dev, "Return code %d\n", rc);
	if (rc)
		goto out_dev_put;
	rc = platform_device_add(part->region);
	xmgmt_info(&lro->pdev->dev, "Return code %d\n", rc);
	if (rc)
		goto out_dev_put;

	rc = xmgmt_subdev_probe(part);
	if (rc)
		goto out_dev_unregister;

	return part;

out_dev_unregister:
	platform_device_unregister(part->region);
out_dev_put:
	platform_device_put(part->region);
out_free:
	vfree(part);
	return ERR_PTR(-ENOMEM);
}

/*
 * Cleanup the regions after their children have been destroyed
 */
static void xmgmt_regions_remove(struct xmgmt_dev *lro)
{
	int i = lro->region_count;
	for (; i > 0;) {
		if (!lro->region[--i])
			continue;
		platform_device_unregister(lro->region[i]->region);
		vfree(lro->region[i]);
		lro->region[i] = NULL;
	}
}

/*
 * The core of this function should be data driven using something like DT
 * Go through each region (static and dynamic) and create the subdevices
 * for the IPs present in the region.
 */
static int xmgmt_regions_probe(struct xmgmt_dev *lro)
{
	int rc = 0;
	struct xmgmt_region *part = NULL;
	part = xmgmt_region_probe(lro, XOCL_REGION_STATIC);
	if (IS_ERR(part))
		return PTR_ERR(part);
	lro->region[0] = part;
	part = xmgmt_region_probe(lro, XOCL_REGION_LEGACYRP);
	if (IS_ERR(part)) {
		rc = PTR_ERR(part);
		goto out_free;
	}
	lro->region[1] = part;
	return 0;
out_free:
	xmgmt_regions_remove(lro);
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

	xmgmt_info(&pdev->dev, "Driver: %s", XMGMT_DRIVER_VERSION);
	xmgmt_info(&pdev->dev, "probe(pdev = 0x%p, pci_id = 0x%p)\n", pdev, id);

	/* Assuming U200 XDMA legacy platform with two regions */
	/* allocate zeroed device book keeping structure */
	lro->region_count = 2;
	lro = xmgmt_drvinst_alloc(&pdev->dev, offsetof(struct xmgmt_dev, region) +
				  sizeof(struct xmgmt_region *) * lro->region_count);
	if (!lro) {
		xmgmt_err(&pdev->dev, "Could not kzalloc(xmgmt_dev).\n");
		rc = -ENOMEM;
		goto err_alloc;
	}

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

	rc = xmgmt_regions_probe(lro);
	if (rc)
		goto err_region;

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

err_region:
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
	xmgmt_info(&pdev->dev, "remove(0x%p) where pdev->dev.driver_data = 0x%p",
                 pdev, lro);
	BUG_ON(lro->pdev != pdev);
	xmgmt_regions_remove(lro);
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
	int res, i;

	pr_info(XMGMT_MODULE_NAME " init()\n");
	xmgmt_class = class_create(THIS_MODULE, "xmgmt_mgmt");
	if (IS_ERR(xmgmt_class))
		return PTR_ERR(xmgmt_class);

	res = alloc_chrdev_region(&xmgmt_devnode, 0,
				  XMGMT_MAX_DEVICES, XMGMT_MODULE_NAME);
	if (res)
		goto alloc_err;

	/* Need to init sub device driver before pci driver register */
#if 0
	for (i = 0; i < ARRAY_SIZE(drv_reg_funcs); ++i) {
		res = drv_reg_funcs[i]();
		if (res)
			goto drv_init_err;
	}
#endif
	res = pci_register_driver(&xmgmt_driver);
	if (res)
		goto reg_err;

	return 0;

drv_init_err:
reg_err:
#if 0
	for (i--; i >= 0; i--)
		drv_unreg_funcs[i]();
#endif

	unregister_chrdev_region(xmgmt_devnode, XMGMT_MAX_DEVICES);
alloc_err:
	pr_info(XMGMT_MODULE_NAME " init() err\n");
	class_destroy(xmgmt_class);
	return res;
}

static void xmgmt_exit(void)
{
	int i;

	pr_info(XMGMT_MODULE_NAME" exit()\n");
	pci_unregister_driver(&xmgmt_driver);

#if 0
	for (i = ARRAY_SIZE(drv_unreg_funcs) - 1; i >= 0; i--)
		drv_unreg_funcs[i]();
#endif

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
