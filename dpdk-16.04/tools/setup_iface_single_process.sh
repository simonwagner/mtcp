#!/bin/bash

# check if there is only one additional command-line argument
if [ $# -ne 1 ]
then
    echo "Usage:"
    echo "$0 <4th-ip-octet-value>"
    exit 1
fi

# Check if you are root
user=`whoami`
if [ "root" != "$user" ]
then
    echo "You are not root!"
    exit 1
fi

# Create & configure /dev/dpdk-iface
rm -rf /dev/dpdk-iface
mknod /dev/dpdk-iface c 1110 0
chmod 666 /dev/dpdk-iface

# First check whether igb_uio module is already loaded
MODULE="igb_uio"

if lsmod | grep "$MODULE" &> /dev/null ; then
  echo "$MODULE is loaded!"
else
  echo "$MODULE is not loaded!"
  exit 1
fi

# Next check how many devices are there in the system
counter=0
for iface_path in `find /sys/module/igb_uio/drivers/pci:igb_uio/*/net/ -maxdepth 1 -mindepth 1 -print`
do
  iface_name=`basename $iface_path`
  echo "/sbin/ifconfig $iface_name 10.0.$(( $counter )).$1 netmask 255.255.255.0 up"
  /sbin/ifconfig $iface_name 10.0.$(( $counter )).$1 netmask 255.255.255.0 up
  let "counter=$counter + 1"
done
