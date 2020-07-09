rmmod xclmgmt
modprobe fpga_mgr
insmod ./lib/xocl-lib.ko
insmod ./mgmt/xmgmt.ko
