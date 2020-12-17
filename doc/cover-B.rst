Hello,

This is v2 of patch series which adds management physical function driver
for Xilinx Alveo PCIe accelerator cards,
https://www.xilinx.com/products/boards-and-kits/alveo.html
This driver is part of Xilinx Runtime (XRT) open source stack.

The patch series depends on libfdt patches which were posted before:
https://lore.kernel.org/lkml/20201128235659.24679-1-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201128235659.24679-2-sonals@xilinx.com/

ALVEO PLATFORM ARCHITECTURE

Alveo PCIe FPGA based platforms have a static *shell* partition and a partial
re-configurable *user* partition. The shell partition is automatically loaded from
flash when host is booted and PCIe is enumerated by BIOS. Shell cannot be changed
till the next cold reboot. The shell exposes two PCIe physical functions:

1. management physical function
2. user physical function

The patch series includes Documentation/xrt.rst which describes Alveo
platform, XRT driver architecture and deployment model in more detail.

Users compile their high level design in C/C++/OpenCL or RTL into FPGA image
using Vitis https://www.xilinx.com/products/design-tools/vitis/vitis-platform.html
tools. The compiled image is packaged as xclbin and contains partial bitstream
for the user partition and necessary metadata. Users can dynamically swap the
image running on the user partition in order to switch between different workloads.

XRT DRIVERS FOR ALVEO

XRT Linux kernel driver *xmgmt* binds to management physical function of
Alveo platform. The modular driver framework is organized into several
platform drivers which primarily handle the following functionality:

1.  Loading firmware container also called xsabin at driver attach time
2.  Loading of user compiled xclbin with FPGA Manager integration
3.  Clock scaling of image running on user partition
4.  In-band sensors: temp, voltage, power, etc.
5.  Device reset and rescan
6.  Flash upgrade of static *shell* partition

The platform drivers are packaged into *xrt-lib* helper module with a well
defined interfaces the details of which can be found in Documentation/xrt.rst.

User physical function driver is not included in this patch series.

TESTING AND VALIDATION

xmgmt driver can be tested with full XRT open source stack which includes
user space libraries, board utilities and (out of tree) first generation
user physical function driver xocl. XRT open source runtime stack is
available at https://github.com/Xilinx/XRT

Complete documentation for XRT open source stack including sections on
Alveo/XRT security and platform architecture can be found here:
https://xilinx.github.io/XRT/master/html/index.html
https://xilinx.github.io/XRT/master/html/security.html
https://xilinx.github.io/XRT/master/html/platforms_partitions.html

Changes since v1:
- Updated the driver to use fpga_region and fpga_bridge for FPGA
  programming
- Dropped subdev drivers not related to PR programming to focus on XRT
  core framework
- Updated Documentation/fpga/xrt.rst with information on XRT core framework
- Addressed checkpatch issues in a few files

For reference V1 version of patch series can be found here--
https://lore.kernel.org/lkml/20201129000040.24777-1-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-2-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-3-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-4-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-5-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-6-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-7-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-8-sonals@xilinx.com/
https://lore.kernel.org/lkml/20201129000040.24777-9-sonals@xilinx.com/

Thanks,
-Sonal
