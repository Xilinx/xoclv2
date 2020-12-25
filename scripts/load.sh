#!/bin/bash -x

rmmod xclmgmt || true
rmmod xmgmt || true
rmmod xrt-lib || true
modprobe fpga_mgr
modprobe fpga_region
modprobe fpga_bridge
insmod ./lib/xrt-lib.ko
insmod ./mgmt/xmgmt.ko

sleep 1

source ~sonals/git/XRT/build/Debug/opt/xilinx/xrt/setup.sh
xbmgmt partition --program --path /lib/firmware/xilinx/862c7020a250293e32036f19956669e5/test/verify.xclbin --force
