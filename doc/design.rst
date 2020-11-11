.. _design.rst:

==================================
XRTV2 Linux Kernel Driver Overview
==================================

XRTV2 drivers are second generation `XRT <https://github.com/Xilinx/XRT>`_ drivers which
support `Alveo <https://www.xilinx.com/products/boards-and-kits/alveo.html>`_ PCIe platforms
from Xilinx.

XRTV2 drivers support *subsystem* style data driven platforms where driver's configuration
and behavior is determined by meta data provided by platform (in *device tree* format).
Primary management physical function (MPF) driver is called **xmgmt**. Primary user physical
function (UPF) driver is called **xuser** and IP drivers are packaged into a library module
called **xrt-lib**, which is shared by **xmgmt** and **xuser**.

Alveo Platform Overview
=======================

Alveo platforms are architected as two physical FPGA partitions: *Shell* and *User*. Shell provides basic
infrastructure for the Alveo platform like PCIe connectivity, board management, Dynamic Function Exchange
(DFX), sensors, clocking, reset, and security. User partition contains user compiled binary which is loaded
by a process called DFX also known as partial reconfiguration.

Physical partitions require strict HW compatibility with each other for DFX to work properly. Every
physical partition has two interface UUIDs: *parent* UUID and *child* UUID. For simple single stage
platforms Shell → User forms parent child relationship. For complex two stage platforms Base → Shell →
User forms the parent child relationship chain.

.. note::
   Partition compatibility matching is key design component of Alveo platforms and XRT. Partitions have child
   and parent relationship. A loaded partition exposes child partition UUID to advertise its compatibility
   requirement for child partition. When loading a child partition the xmgmt management driver matches parent
   UUID of the child partition against child UUID exported by the parent. Parent and child partition UUIDs are
   stored in the xclbin (for user) or xsabin (for base and shell). Except for VSEC UUIDs are stored in xsabin
   or xclbin. The hardware itself does not know about UUIDs.


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
              |				        |
          +---+---+			 +------+--------+
          |  POR  |			 | USER COMPILED |
          | FLASH |			 |    XCLBIN     |
          +-------+			 +---------------+


Loading Sequence
----------------

Shell partition is loaded from flash at system boot time. It establishes the PCIe link and exposes two physical
functions to the BIOS. After OS boot, xmgmt driver attaches to PCIe physical function 0 exposed by the Shell and
then looks for VSEC in PCIe extended configuration space. Using VSEC it determines the logic UUID of Shell and uses
the UUID to load matching *xsabin* file from Linux firmware directory. The xsabin file contains metadata to discover
peripherals that are part of Shell and firmware(s) for any embedded soft processors in Shell.

Shell exports child interface UUID which is used for compatibility check when loading user compiled xclbin over the
User partition as part of DFX. When a user requests loading of a specific xclbin the xmgmt management driver reads
the parent interface UUID specified in the xclbin and matches it with child interface UUID exported by Shell to
determine if xclbin is compatible with the Shell. If match fails loading of xclbin is denied.

xclbin loading is requested using ICAP_DOWNLOAD_AXLF ioctl command. When loading xclbin xmgmt driver performs the
following operations:

1. Sanity check the xclbin contents
2. Isolate the User partition
3. Download the bitstream using the FPGA config engine
4. Program the clocks driving the User region
5. De-isolate the User partition

`Platform Loading Overview <https://xilinx.github.io/XRT/master/html/platforms_partitions.html>`_ provides more
detailed information on platform loading.

xclbin
------

xclbin is ELF-like binary container format.

Deployment Models
=================

Baremetal
---------

In baremetal deployments both MPF and UPF are visible and accessible. xmgmt driver binds to
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

In virtualized deployments privileged MPF is assigned to host but unprivileged UPF is assigned to
guest VM via PCIe pass-through. xmgmt driver in host binds to MPF. xmgmt driver operations are privileged
and available to system administrator. The full stack is illustrated below::


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

Repository of all IP subsystem drivers and pure software modules that can potentially
be shared between xmgmt and xuser. All these drivers are Linux *platform driver*
that are instantiated by xmgmt (or xuser in future) based on meta data associated with
hardware.

xmgmt.ko
--------

The xmgmt driver is a PCIe device driver driving MPF found on Xilinx's Alveo
PCIE device. It consists of one *root* driver, one or more *partition* drivers and
one or more *leaf* drivers. The root and MPF specific leaf drivers are in
xmgmt.ko. The partition driver and other leaf drivers are in xrt-lib.ko.

The instantiation of specific partition driver or leaf driver is completely data
driven based on meta data (mostly in device tree format) found through VSEC
capability and inside firmware files, such as XSABIN or XCLBIN file. The root
driver manages life cycle of multiple partition drivers, which, in turn, manages
multiple leaf drivers. This allows a single set of driver code to support all
kinds of IP subsystems exposed by different shells. The difference among all
these IP subsystems will be handled in leaf drivers with root and partition drivers being
part of the infrastructure and provide common services for all leaves found on
all platforms.


xmgmt-root
^^^^^^^^^^

The xmgmt-root driver is a PCIe device driver attaches to MPF. It's part of the
infrastructure of the MPF driver and resides in xmgmt.ko. This driver

* manages one or more partition drivers
* provides access to functionalities that requires pci_dev, such as PCIE config
  space access, to other leaf drivers through parent calls
* together with partition driver, facilities event callbacks for other leaf drivers
* together with partition driver, facilities inter-leaf driver calls for other leaf drivers

When root driver starts, it will explicitly create an initial partition instance,
which contains leaf drivers that will trigger the creation of other partition
instances. The root driver will wait for all partitions and leaves to be created
before it returns from it's probe routine and claim success of the initialization of the
entire xmgmt driver.

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

Leaf drivers assigned to specific hardware resources drive specific IP subsystem in
the device. To manipulate the IP subsystem or carry out a task, a leaf driver may ask
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

xmgmt driver exposes a rich set of sysfs interfaces. IP subsystem platform drivers
export sysfs node for every platform instance.

Every partition also exports its UUIDs. See below for examples::

  /sys/bus/pci/devices/0000:06:00.0/xmgmt_main.0/interface_uuids
  /sys/bus/pci/devices/0000:06:00.0/xmgmt_main.0/logic_uuids


hwmon
-----

xmgmt driver expoes standard hwmon interface to report voltage, current, temperature,
power, etc. These can easily be viewed using *sensors* command line utility.


Platform Security Considerations
================================

`Security of Alveo Platform <https://xilinx.github.io/XRT/master/html/security.html>`_
discusses the deployment options and security implications in great detail.
