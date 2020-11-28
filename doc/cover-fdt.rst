Hello,

This patch series adds support for exporting limited set of libfdt symbols from
Linux kernel. This enables drivers and other kernel modules to use libfdt for
working with device trees. This may be used by platform vendors to describe HW
features inside a PCIe device to its driver.


Xilinx Alveo PCIe accelerator card driver patch series which follows this patch
makes use of device tree to advertise HW subsystems sitting behind PCIe BARs.
The use of device trees makes the driver data driven and overall solution more
scalable.

Thanks,
-Sonal
