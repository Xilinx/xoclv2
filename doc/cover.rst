Hello,

This patch series adds management physical function driver for Xilinx Alveo PCIe
accelerator cards.
https://www.xilinx.com/products/boards-and-kits/alveo.html
The driver is part of Xilinx Runtime (XRT) open source stack.

The patch depends on the previous Device Tree patch series.

PLATFORM ARCHITECTURE

Alveo PCIe FPGA based platforms have a static *shell* partition and a partial
re-configurable *user* partition. The shell is automatically loaded from PROM
when host is booted and PCIe is enumerated by BIOS. Shell cannot be changed till
next cold reboot. The shell exposes two PCIe physical functions:

1. management physical function
2. user physical function

The patch series includes Documentation patch, xrt.rst which describes Alveo
platform, xmgmt driver architecture and deployment model in more more detail.

Users compile their high level design in C/C++/OpenCL or RTL into FPGA image
using Vitis https://www.xilinx.com/products/design-tools/vitis/vitis-platform.html
tools. The image is packaged as xclbin and contains partial bitstream for the
user partition and necessary metadata. Users can dynamically swap the image
running on the user partition in order to switch between different workloads.

XRT DRIVERS

XRT Linux kernel driver xmgmt binds to mgmt physical function of Alveo platform.
The modular driver framework is organized into several platform drivers which
primarily handle the following functionality:

1.  Loading firmware container also called xsabin at driver attach time
2.  Loading of user compiled xclbin with FPGA Manager integration
3.  Clock scaling of image running on user partition
4.  In-band sensors: temp, voltage, power, etc.
5.  Device reset and rescan
6.  Flashing static *shell* partition

More details on driver architecture can be found in the included xrt.rst
documentation.

xmgmt driver is second generation XRT management driver and evolution of
the first generation (out of tree) driver XRT management driver called
xclmgmt.

TESTING AND VALIDATION

xmgmt driver can be tested with full XRT open source stack which includes
user space libraries, board utilities and (out of tree) first generation
user physical function driver xocl. XRT open source runtime stack is
available at https://github.com/Xilinx/XRT

XRT is documented at https://xilinx.github.io/XRT/master/html/index.html
