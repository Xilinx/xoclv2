.. _design.rst:

XRTV2 Linux Kernel Driver Overview
**********************************

XRTV2 drivers are second generation `XRT <https://github.com/Xilinx/XRT>`_ drivers which
support `Alveo <https://www.xilinx.com/products/boards-and-kits/alveo.html>`_ PCIe platforms
from Xilinx.

XRTV2 drivers support *subsystem* style data driven platforms where driver's configuration
and behavior is determined by meta data provided by platform (in *device tree* format).
Primary management physical function (MPF) driver is called **xmgmt**. Primary user physical
function (UPF) driver is called **xuser** and IP drivers are packaged into a library module
called **xrt-lib**, which is shared by **xmgmt** and **xuser**.

Driver Modules
==============

xrt-lib.ko
-----------

Repository of all IP drivers and pure software modules that can potentially be
shared between xmgmt and xuser. All these drivers are Linux *platform drivers*
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
kinds of MPFs exposed by different BLPs, PLPs and ULPs. The difference among all
these MPFs will be handled in leaf drivers with root and partition drivers being
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
groups of leaves found on different partitions on hardware, such as VSEC, BLP,
PLP and ULP.

leaves
^^^^^^

The leaf driver is a platform device driver whose life cycle is managed by
a partition driver and may or may not have real IO mem or IRQ resources. They
are the real meat of xmgmt and contains platform specific code to BLP/PLP/ULP
found on a MPF.

A leaf driver may not have real hardware resources when it merely acts as a driver
that manages certain in-memory states for xmgmt. These in-memory states could be
shared by multiple other leaves.

Leaf drivers assigned to specific hardware resources drive specific IPs in the device.
To manipulate the IP or carry out a task, a leaf driver may ask help from root via
parent calls and/or from other leaves via inter-leaf calls.

A leaf can also broadcast events through infrastructure code for other leaves
to process. It can also receive event notification from infrastructure about certain
events, such as post-creation or pre-exit of a particular leaf.

Platform Security Considerations
================================
