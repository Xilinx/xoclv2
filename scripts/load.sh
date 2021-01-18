#!/bin/bash -x

set -e

# Reload xmgmt driver
rmmod xclmgmt || true
rmmod xmgmt || true
rmmod xrt-stest1 || true
rmmod xrt-lib || true
modprobe fpga_mgr
modprobe fpga_region
modprobe fpga_bridge
insmod ./lib/xrt-lib.ko
insmod ./mgmt/xmgmt.ko

sleep 1

# Validate 'sensors' can find us
sensors xilinx_u50_gen3x16_xdma_201920_3-pci-0600

# Validate xclbin loading verify.xclbin
if [[ ! -x $XILINX_XRT/bin/xbmgmt ]]
then
   source /opt/xilinx/xrt/setup.sh
fi
xbmgmt partition --program --path /lib/firmware/xilinx/862c7020a250293e32036f19956669e5/test/verify.xclbin --force
tree -f -l -L 2 /sys/class/fpga_bridge/
tree -f -l -L 2 /sys/class/fpga_manager/
tree -f -l -L 2 /sys/class/fpga_region/

sudo rmmod xmgmt
sleep 1
sudo insmod selftests/xrt-stest1.ko
