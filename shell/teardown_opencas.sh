#!/bin/bash

# Check if script is run as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (sudo)"
    exit 1
fi

# Verify if a cache instance exists
CACHE_INSTANCE=$(casadm -L | grep "^cache" | awk '{print $2}')
if [ -n "$CACHE_INSTANCE" ]; then
    echo "Stopping existing cache instance..."
    casadm -T -i "$CACHE_INSTANCE"
    if [ $? -ne 0 ]; then
        echo "Failed to stop cache instance"
        exit 1
    fi
    echo "Cache instance stopped successfully"
else
    echo "No cache instance found"
fi

# Verify if a core device is attached
CORE_DEVICE=$(casadm -L | grep "Core Id" | awk '{print $4}')
if [ -n "$CORE_DEVICE" ]; then
    echo "Removing core device..."
    casadm -R -i "$CACHE_INSTANCE" -j "$CORE_DEVICE"
    if [ $? -ne 0 ]; then
        echo "Failed to remove core device"
        exit 1
    fi
    echo "Core device removed successfully"
else
    echo "No core device found"
fi

# Remove RAM disk
if [ -e /dev/ram1 ]; then
    echo "Removing RAM disk..."
    rm -f /dev/disk/by-id/ramdisk1
    rmmod brd
    if [ $? -ne 0 ]; then
        echo "Failed to remove RAM disk"
        exit 1
    fi
    echo "RAM disk removed successfully"
else
    echo "No RAM disk found"
fi

# Remove NVMe metadata
if [ -e /dev/nvme1n1 ]; then
    echo "Cleaning NVMe device metadata..."
    if /home/chanseo/shell/disconnect-nvme.sh; then
        echo "NVMe device is disconected successfully."
    else
        echo "NVMe disconection failed."
    fi
else
    echo "No NVMe device found at /dev/nvme1n1"
fi  # <-- 이 부분이 누락되었던 fi입니다.

# Final verification
echo "Verifying status..."
casadm -L

if [ $? -eq 0 ]; then
    echo "Teardown completed successfully!"
else
    echo "Teardown encountered issues. Please check manually."
fi