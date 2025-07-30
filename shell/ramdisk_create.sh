#!/bin/bash
#10GB RAM Disk
modprobe brd rd_size=10485760
ln -s /dev/ram1 /dev/disk/by-id/ramdisk1

# Verify RAM disk creation
if [ -e /dev/ram1 ]; then
    echo "RAM disk /dev/ram1 created successfully"
    ls -lh /dev/ram1
else
    echo "Failed to create RAM disk"
    exit 1
fi

# Verify symbolic link creation
if [ -L /dev/disk/by-id/ramdisk1 ]; then
    echo "Symbolic link created successfully"
    ls -l /dev/disk/by-id/ramdisk1
else
    echo "Failed to create symbolic link"
    exit 1
fi
