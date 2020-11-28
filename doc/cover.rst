Hello,

This patch series adds management physical function driver for Xilinx Alveo PCIe
accelerator cards, https://www.xilinx.com/products/boards-and-kits/alveo.html
This driver is part of Xilinx Runtime (XRT) open source stack.

The patch depends on the libfdt patch which was posted before.

ALVEO PLATFORM ARCHITECTURE

Alveo PCIe FPGA based platforms have a static *shell* partition and a partial
re-configurable *user* partition. The shell partition is automatically loaded from
flash when host is booted and PCIe is enumerated by BIOS. Shell cannot be changed
till the next cold reboot. The shell exposes two PCIe physical functions:

1. management physical function
2. user physical function

The patch series includes Documentation/xrt.rst which describes Alveo
platform, xmgmt driver architecture and deployment model in more more detail.

Users compile their high level design in C/C++/OpenCL or RTL into FPGA image
using Vitis https://www.xilinx.com/products/design-tools/vitis/vitis-platform.html
tools. The image is packaged as xclbin and contains partial bitstream for the
user partition and necessary metadata. Users can dynamically swap the image
running on the user partition in order to switch between different workloads.

ALVEO DRIVERS

Alveo Linux kernel driver *xmgmt* binds to management physical function of
Alveo platform. The modular driver framework is organized into several
platform drivers which primarily handle the following functionality:

1.  Loading firmware container also called xsabin at driver attach time
2.  Loading of user compiled xclbin with FPGA Manager integration
3.  Clock scaling of image running on user partition
4.  In-band sensors: temp, voltage, power, etc.
5.  Device reset and rescan
6.  Flashing static *shell* partition

The platform drivers are packaged into *xrt-lib* helper module with a well
defined interfaces the details of which can be found in Documentation/xrt.rst.

xmgmt driver is second generation Alveo management driver and evolution of
the first generation (out of tree) Alveo management driver, xclmgmt. The
sources of the first generation drivers were posted on LKML last year--
https://lore.kernel.org/lkml/20190319215401.6562-1-sonal.santan@xilinx.com/

Changes since the first generation driver include the following: the driver
has been re-architected as data driven modular driver; the driver has been
split into xmgmt and xrt-lib; user physical function driver has been removed
from the driver suite.

Detailed documentation on Alveo/XRT security and platform architecture has
been published:
https://xilinx.github.io/XRT/master/html/security.html
https://xilinx.github.io/XRT/master/html/platforms_partitions.html

User physical function driver is not included in this patch series.

TESTING AND VALIDATION

xmgmt driver can be tested with full XRT open source stack which includes
user space libraries, board utilities and (out of tree) first generation
user physical function driver xocl. XRT open source runtime stack is
available at https://github.com/Xilinx/XRT

Complete documentation for XRT open source stack can be found here--
https://xilinx.github.io/XRT/master/html/index.html

Thanks,
-Sonal
