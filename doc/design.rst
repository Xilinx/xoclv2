.. _design.rst:

XRT Linux Kernel Driver XOCLV2
*******************************

V2 driver will only support SSv2 style platforms. Platforms using legacy methodology would still
need to provide xsabin with device tree and UUIDs for drivers' consumption. Primary UPF driver
is called **xuser** and primary MPF driver is called **xmgmt**. IP drivers are packaged into a
library called **xocl-lib**.

Driver Modules
==============

xocl-lib.ko
-----------

Repository of all IP drivers. IP drivers are modeled as ``xocl_subdev_base`` and ``xocl_subdev_drv``.
xocl_subdev_base represents the IP instance and xocl_subdev_drv represents the services offered by
the IP. File ``xocl-lib.h`` defines the data structures. None of the APIs are exposed to user space.

xmgmt-fmr.ko
------------

Linux FPGA Manager integration with Linux standard in-kernel -- no user space APIs -- xclbin download APIs. V2 drivers require
FPGA Manager support in Linux kernel.

xmgmt.ko
--------

xmgmt is the primary management driver exposed to end-users. The driver attempts to expose the current xclmgmt
driver ioctls to user space. It loads the xsabin, reads the device-tree, creates the partitions which are
modeled as ``xocl_region``. The driver also creates collection of *child* xocl_subdev_base nodes by reading IP
instantiations in device-tree.

xuser.ko
--------

xmgmt is the primary driver exposed to end-users. Need to decide if KDS and MM will be part of this or will they
be separate modules.

xuser-xdma.ko
-------------

TBD

xuser-qdma.ko
-------------

TBD


Driver Model
============

.. include:: ../core/relations.txt


.. code-block:: c

   // Define file operations (if any)
   static const struct file_operations icap_fops = {
	.open = icap_open,
	.release = icap_close,
	.write = icap_write_rp,
   };

   // Define standard operations for the subdev driver. The framework (xocl-lib)
   // will perform basic initializations: allocating chrdev region, initializing IDA,
   // etc. including calling any driver specific post-init hook. The framework also
   // does the teardown during unload time including calling any driver specific
   // pre-exit hook. See xocl_iplib_init() in xocl-lib for details.
   static struct xocl_subdev_drv icap_ops = {
	.ioctl = icap_ioctl,
	.fops = &icap_fops,
	.id = XOCL_SUBDEV_ICAP,
	.dnum = -1,
	.drv_post_init = xocl_post_init_icap,
	.drv_pre_exit = xocl_pre_exit_icap,
   };

   // Attach the subdevice driver to the platform
   static const struct platform_device_id icap_id_table[] = {
	{ XOCL_ICAP, (kernel_ulong_t)&icap_ops },
	{ },
   };


   // Advertise platform driver; this lets xmgmt to discover this
   // driver by looking up by name
   struct platform_driver xocl_icap_driver = {
	.driver	= {
		.name    = XOCL_ICAP,
	},
	.probe    = xocl_icap_probe,
	.remove   = xocl_icap_remove,
	.id_table = icap_id_table,
   };
