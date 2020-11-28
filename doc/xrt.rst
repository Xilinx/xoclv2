==================================
XRTV2 Linux Kernel Driver Overview
==================================

XRTV2 drivers are second generation `XRT <https://github.com/Xilinx/XRT>`_ drivers which
support `Alveo <https://www.xilinx.com/products/boards-and-kits/alveo.html>`_ PCIe platforms
from Xilinx.

XRTV2 drivers support *subsystem* style data driven platforms where driver's configuration
and behavior is determined by meta data provided by platform (in *device tree* format).
Primary management physical function (MPF) driver is called **xmgmt**. Primary user physical
function (UPF) driver is called **xuser** and HW subsystem drivers are packaged into a library
module called **xrt-lib**, which is shared by **xmgmt** and **xuser** (WIP).

Alveo Platform Overview
=======================

Alveo platforms are architected as two physical FPGA partitions: *Shell* and *User*. Shell
provides basic infrastructure for the Alveo platform like PCIe connectivity, board management,
Dynamic Function Exchange (DFX), sensors, clocking, reset, and security. User partition contains
user compiled binary which is loaded by a process called DFX also known as partial reconfiguration.

Physical partitions require strict HW compatibility with each other for DFX to work properly.
Every physical partition has two interface UUIDs: *parent* UUID and *child* UUID. For simple
single stage platforms Shell → User forms parent child relationship. For complex two stage
platforms Base → Shell → User forms the parent child relationship chain.

.. note::
   Partition compatibility matching is key design component of Alveo platforms and XRT. Partitions
   have child and parent relationship. A loaded partition exposes child partition UUID to advertise
   its compatibility requirement for child partition. When loading a child partition the xmgmt
   management driver matches parent UUID of the child partition against child UUID exported by the
   parent. Parent and child partition UUIDs are stored in the *xclbin* (for user) or *xsabin* (for
   base and shell). Except for root UUID, VSEC, hardware itself does not know about UUIDs. UUIDs are
   stored in xsabin and xclbin.


The physical partitions and their loading is illustrated below::

           SHELL                               USER
        +-----------+                  +-------------------+
        |           |                  |                   |
        | VSEC UUID | CHILD     PARENT |    LOGIC UUID     |
        |           o------->|<--------o                   |
        |           | UUID       UUID  |                   |
        +-----+-----+                  +--------+----------+
              |                                 |
              .                                 .
              |                                 |
          +---+---+                      +------+--------+
          |  POR  |                      | USER COMPILED |
          | FLASH |                      |    XCLBIN     |
          +-------+                      +---------------+


Loading Sequence
----------------

Shell partition is loaded from flash at system boot time. It establishes the PCIe link and exposes
two physical functions to the BIOS. After OS boot, xmgmt driver attaches to PCIe physical function
0 exposed by the Shell and then looks for VSEC in PCIe extended configuration space. Using VSEC it
determines the logic UUID of Shell and uses the UUID to load matching *xsabin* file from Linux
firmware directory. The xsabin file contains metadata to discover peripherals that are part of Shell
and firmware(s) for any embedded soft processors in Shell.

Shell exports child interface UUID which is used for compatibility check when loading user compiled
xclbin over the User partition as part of DFX. When a user requests loading of a specific xclbin the
xmgmt management driver reads the parent interface UUID specified in the xclbin and matches it with
child interface UUID exported by Shell to determine if xclbin is compatible with the Shell. If match
fails loading of xclbin is denied.

xclbin loading is requested using ICAP_DOWNLOAD_AXLF ioctl command. When loading xclbin xmgmt driver
performs the following operations:

1. Sanity check the xclbin contents
2. Isolate the User partition
3. Download the bitstream using the FPGA config engine (ICAP)
4. De-isolate the User partition
5. Program the clocks (ClockWiz) driving the User partition
6. Wait for memory controller (MIG) calibration

`Platform Loading Overview <https://xilinx.github.io/XRT/master/html/platforms_partitions.html>`_
provides more detailed information on platform loading.

xsabin
------

Each Alveo platform comes packaged with its own xsabin. The xsabin is trusted component of the
platform. For format details refer to :ref:`xsabin/xclbin Container Format`. xsabin contains
basic information like UUIDs, platform name and metadata in the form of device tree. See
:ref:`Device Tree Usage` for details and example.

xclbin
------

xclbin is compiled by end user using
`Vitis <https://www.xilinx.com/products/design-tools/vitis/vitis-platform.html>`_ tool set from
Xilinx. The xclbin contains sections describing user compiled acceleration engines/kernels, memory
subsystems, clocking information etc. It also contains bitstream for the user partition, UUIDs,
platform name, etc. xclbin uses the same container format as xsabin which is described below.


xsabin/xclbin Container Format
------------------------------

xclbin/xsabin is ELF-like binary container format. It is structured as series of sections.
There is a file header followed by several section headers which is followed by sections.
A section header points to an actual section. There is an optional signature at the end.
The format is defined by header file ``xclbin.h``. The following figure illustrates a
typical xclbin::


           +---------------------+
           |                     |
           |       HEADER        |
           +---------------------+
           |   SECTION  HEADER   |
           |                     |
           +---------------------+
           |         ...         |
           |                     |
           +---------------------+
           |   SECTION  HEADER   |
           |                     |
           +---------------------+
           |       SECTION       |
           |                     |
           +---------------------+
           |         ...         |
           |                     |
           +---------------------+
           |       SECTION       |
           |                     |
           +---------------------+
           |      SIGNATURE      |
           |      (OPTIONAL)     |
           +---------------------+


xclbin/xsabin files can be packaged, un-packaged and inspected using XRT utility called
**xclbinutil**. xclbinutil is part of XRT open source software stack. The source code for
xclbinutil can be found at https://github.com/Xilinx/XRT/tree/master/src/runtime_src/tools/xclbinutil

For example to enumerate the contents of a xclbin/xsabin use the *--info* switch as shown
below::

  xclbinutil --info --input /opt/xilinx/firmware/u50/gen3x16-xdma/blp/test/bandwidth.xclbin
  xclbinutil --info --input /lib/firmware/xilinx/862c7020a250293e32036f19956669e5/partition.xsabin


Device Tree Usage
-----------------

As mentioned previously xsabin stores metadata which advertise HW subsystems present in a partition.
The metadata is stored in device tree format with well defined schema. Subsystem instantiations are
captured as children of ``addressable_endpoints`` node. Subsystem nodes have standard attributes like
``reg``, ``interrupts`` etc. Additionally the nodes also have PCIe specific attributes:
``pcie_physical_function`` and ``pcie_bar_mapping``. These identify which PCIe physical function and
which BAR space in that physical function the subsystem resides. XRT management driver uses this
information to bind *platform drivers* to the subsystem instantiations. The platform drivers are
found in **xrt-lib.ko** kernel module defined later. Below is an example of device tree for Alveo U50
platform::

  /dts-v1/;

  /{
        logic_uuid = "f465b0a3ae8c64f619bc150384ace69b";

        schema_version {
                major = <0x01>;
                minor = <0x00>;
        };

        interfaces {

                @0 {
                        interface_uuid = "862c7020a250293e32036f19956669e5";
                };
        };

        addressable_endpoints {

                ep_blp_rom_00 {
                        reg = <0x00 0x1f04000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_bram_ctrl-1.0\0axi_bram_ctrl";
                };

                ep_card_flash_program_00 {
                        reg = <0x00 0x1f06000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_quad_spi-1.0\0axi_quad_spi";
                        interrupts = <0x03 0x03>;
                };

                ep_cmc_firmware_mem_00 {
                        reg = <0x00 0x1e20000 0x00 0x20000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_bram_ctrl-1.0\0axi_bram_ctrl";

                        firmware {
                                firmware_product_name = "cmc";
                                firmware_branch_name = "u50";
                                firmware_version_major = <0x01>;
                                firmware_version_minor = <0x00>;
                        };
                };

                ep_cmc_intc_00 {
                        reg = <0x00 0x1e03000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_intc-1.0\0axi_intc";
                        interrupts = <0x04 0x04>;
                };

                ep_cmc_mutex_00 {
                        reg = <0x00 0x1e02000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_cmc_regmap_00 {
                        reg = <0x00 0x1e08000 0x00 0x2000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_bram_ctrl-1.0\0axi_bram_ctrl";

                        firmware {
                                firmware_product_name = "sc-fw";
                                firmware_branch_name = "u50";
                                firmware_version_major = <0x05>;
                        };
                };

                ep_cmc_reset_00 {
                        reg = <0x00 0x1e01000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_ddr_mem_calib_00 {
                        reg = <0x00 0x63000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_debug_bscan_mgmt_00 {
                        reg = <0x00 0x1e90000 0x00 0x10000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-debug_bridge-1.0\0debug_bridge";
                };

                ep_ert_base_address_00 {
                        reg = <0x00 0x21000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_ert_command_queue_mgmt_00 {
                        reg = <0x00 0x40000 0x00 0x10000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-ert_command_queue-1.0\0ert_command_queue";
                };

                ep_ert_command_queue_user_00 {
                        reg = <0x00 0x40000 0x00 0x10000>;
                        pcie_physical_function = <0x01>;
                        compatible = "xilinx.com,reg_abs-ert_command_queue-1.0\0ert_command_queue";
                };

                ep_ert_firmware_mem_00 {
                        reg = <0x00 0x30000 0x00 0x8000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_bram_ctrl-1.0\0axi_bram_ctrl";

                        firmware {
                                firmware_product_name = "ert";
                                firmware_branch_name = "v20";
                                firmware_version_major = <0x01>;
                        };
                };

                ep_ert_intc_00 {
                        reg = <0x00 0x23000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_intc-1.0\0axi_intc";
                        interrupts = <0x05 0x05>;
                };

                ep_ert_reset_00 {
                        reg = <0x00 0x22000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_ert_sched_00 {
                        reg = <0x00 0x50000 0x00 0x1000>;
                        pcie_physical_function = <0x01>;
                        compatible = "xilinx.com,reg_abs-ert_sched-1.0\0ert_sched";
                        interrupts = <0x09 0x0c>;
                };

                ep_fpga_configuration_00 {
                        reg = <0x00 0x1e88000 0x00 0x8000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_hwicap-1.0\0axi_hwicap";
                        interrupts = <0x02 0x02>;
                };

                ep_icap_reset_00 {
                        reg = <0x00 0x1f07000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_mailbox_mgmt_00 {
                        reg = <0x00 0x1f10000 0x00 0x10000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-mailbox-1.0\0mailbox";
                        interrupts = <0x00 0x00>;
                };

                ep_mailbox_user_00 {
                        reg = <0x00 0x1f00000 0x00 0x10000>;
                        pcie_physical_function = <0x01>;
                        compatible = "xilinx.com,reg_abs-mailbox-1.0\0mailbox";
                        interrupts = <0x08 0x08>;
                };

                ep_msix_00 {
                        reg = <0x00 0x00 0x00 0x20000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-msix-1.0\0msix";
                        pcie_bar_mapping = <0x02>;
                };

                ep_pcie_link_mon_00 {
                        reg = <0x00 0x1f05000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_pr_isolate_plp_00 {
                        reg = <0x00 0x1f01000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_pr_isolate_ulp_00 {
                        reg = <0x00 0x1000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_gpio-1.0\0axi_gpio";
                };

                ep_uuid_rom_00 {
                        reg = <0x00 0x64000 0x00 0x1000>;
                        pcie_physical_function = <0x00>;
                        compatible = "xilinx.com,reg_abs-axi_bram_ctrl-1.0\0axi_bram_ctrl";
                };

                ep_xdma_00 {
                        reg = <0x00 0x00 0x00 0x10000>;
                        pcie_physical_function = <0x01>;
                        compatible = "xilinx.com,reg_abs-xdma-1.0\0xdma";
                        pcie_bar_mapping = <0x02>;
                };
        };

  }



Deployment Models
=================

Baremetal
---------

In bare-metal deployments both MPF and UPF are visible and accessible. xmgmt driver binds to
MPF. xmgmt driver operations are privileged and available to system administrator. The full
stack is illustrated below::


                            HOST

                 [XMGMT]            [XUSER]
                    |                  |
                    |                  |
                 +-----+            +-----+
                 | MPF |            | UPF |
                 |     |            |     |
                 | PF0 |            | PF1 |
                 +--+--+            +--+--+
          ......... ^................. ^..........
                    |                  |
                    |   PCIe DEVICE    |
                    |                  |
                 +--+------------------+--+
                 |         SHELL          |
                 |                        |
                 +------------------------+
                 |         USER           |
                 |                        |
                 |                        |
                 |                        |
                 |                        |
                 +------------------------+



Virtualized
-----------

In virtualized deployments privileged MPF is assigned to host but unprivileged UPF
is assigned to guest VM via PCIe pass-through. xmgmt driver in host binds to MPF.
xmgmt driver operations are privileged and only accessible by hosting service provider.
The full stack is illustrated below::


                                 .............
                  HOST           .    VM     .
                                 .           .
                 [XMGMT]         .  [XUSER]  .
                    |            .     |     .
                    |            .     |     .
                 +-----+         .  +-----+  .
                 | MPF |         .  | UPF |  .
                 |     |         .  |     |  .
                 | PF0 |         .  | PF1 |  .
                 +--+--+         .  +--+--+  .
          ......... ^................. ^..........
                    |                  |
                    |   PCIe DEVICE    |
                    |                  |
                 +--+------------------+--+
                 |         SHELL          |
                 |                        |
                 +------------------------+
                 |         USER           |
                 |                        |
                 |                        |
                 |                        |
                 |                        |
                 +------------------------+



Driver Modules
==============

xrt-lib.ko
----------

Repository of all subsystem drivers and pure software modules that can potentially
be shared between xmgmt and xuser. All these drivers are structured as Linux
*platform driver* and are instantiated by xmgmt (or xuser in future) based on meta
data associated with hardware. The metadata is in the form of device tree as
explained before.

xmgmt.ko
--------

The xmgmt driver is a PCIe device driver driving MPF found on Xilinx's Alveo
PCIE device. It consists of one *root* driver, one or more *partition* drivers
and one or more *leaf* drivers. The root and MPF specific leaf drivers are in
xmgmt.ko. The partition driver and other leaf drivers are in xrt-lib.ko.

The instantiation of specific partition driver or leaf driver is completely data
driven based on meta data (mostly in device tree format) found through VSEC
capability and inside firmware files, such as xsabin or xclbin file. The root
driver manages life cycle of multiple partition drivers, which, in turn, manages
multiple leaf drivers. This allows a single set of driver code to support all
kinds of subsystems exposed by different shells. The difference among all
these subsystems will be handled in leaf drivers with root and partition drivers
being part of the infrastructure and provide common services for all leaves found
on all platforms.


xmgmt-root
^^^^^^^^^^

The xmgmt-root driver is a PCIe device driver attaches to MPF. It's part of the
infrastructure of the MPF driver and resides in xmgmt.ko. This driver

* manages one or more partition drivers
* provides access to functionalities that requires pci_dev, such as PCIE config
  space access, to other leaf drivers through parent calls
* together with partition driver, facilities event callbacks for other leaf drivers
* together with partition driver, facilities inter-leaf driver calls for other leaf
  drivers

When root driver starts, it will explicitly create an initial partition instance,
which contains leaf drivers that will trigger the creation of other partition
instances. The root driver will wait for all partitions and leaves to be created
before it returns from it's probe routine and claim success of the initialization
of the entire xmgmt driver.

partition
^^^^^^^^^

The partition driver is a platform device driver whose life cycle is managed by
root and does not have real IO mem or IRQ resources. It's part of the
infrastructure of the MPF driver and resides in xrt-lib.ko. This driver

* manages one or more leaf drivers so that multiple leaves can be managed as a group
* provides access to root from leaves, so that parent calls, event notifications
  and inter-leaf calls can happen

In xmgmt, an initial partition driver instance will be created by root, which
contains leaves that will trigger partition instances to be created to manage
groups of leaves found on different partitions on hardware, such as VSEC, Shell,
and User.

leaves
^^^^^^

The leaf driver is a platform device driver whose life cycle is managed by
a partition driver and may or may not have real IO mem or IRQ resources. They
are the real meat of xmgmt and contains platform specific code to Shell and User
found on a MPF.

A leaf driver may not have real hardware resources when it merely acts as a driver
that manages certain in-memory states for xmgmt. These in-memory states could be
shared by multiple other leaves.

Leaf drivers assigned to specific hardware resources drive specific subsystem in
the device. To manipulate the subsystem or carry out a task, a leaf driver may ask
help from root via parent calls and/or from other leaves via inter-leaf calls.

A leaf can also broadcast events through infrastructure code for other leaves
to process. It can also receive event notification from infrastructure about certain
events, such as post-creation or pre-exit of a particular leaf.


Driver Interfaces
=================

xmgmt Driver Ioctls
-------------------

Ioctls exposed by xmgmt driver to user space are enumerated in the following table:

== ===================== ============================= ===========================
#  Functionality         ioctl request code            data format
== ===================== ============================= ===========================
1  FPGA image download   XMGMT_IOCICAPDOWNLOAD_AXLF    xmgmt_ioc_bitstream_axlf
2  CL frequency scaling  XMGMT_IOCFREQSCALE            xmgmt_ioc_freqscaling
== ===================== ============================= ===========================

xmgmt Driver Sysfs
------------------

xmgmt driver exposes a rich set of sysfs interfaces. Subsystem platform drivers
export sysfs node for every platform instance.

Every partition also exports its UUIDs. See below for examples::

  /sys/bus/pci/devices/0000:06:00.0/xmgmt_main.0/interface_uuids
  /sys/bus/pci/devices/0000:06:00.0/xmgmt_main.0/logic_uuids


hwmon
-----

xmgmt driver exposes standard hwmon interface to report voltage, current, temperature,
power, etc. These can easily be viewed using *sensors* command line utility.


mailbox
-------

xmgmt communicates with user physical function driver via HW mailbox. Mailbox opcodes
are defined in ``mailbox_proto.h``. `Mailbox Inter-domain Communication Protocol
<https://xilinx.github.io/XRT/master/html/mailbox.proto.html>`_ defines the full
specification. xmgmt implements subset of the specification. It provides the following
services to the UPF driver:

1.  Responding to *are you there* request including determining if the two drivers are
    running in the same OS domain
2.  Provide sensor readings, loaded xclbin UUID, clock frequency, shell information, etc.
3.  Perform PCIe hot reset
4.  Download user compiled xclbin


Platform Security Considerations
================================

`Security of Alveo Platform <https://xilinx.github.io/XRT/master/html/security.html>`_
discusses the deployment options and security implications in great detail.
