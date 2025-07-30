#!/bin/bash

# Check if script is run as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (sudo)"
    exit 1
fi


# Change cache mode to pmt
echo "Changing cache mode to pmt..."
if ! sudo casadm -Q -i 1 -c pt; then
    echo "Failed to change cache mode"
    exit 1
fi

# Stop netCAS Thread
echo "Stopping netCAS Thread..."
if ! sudo casadm -N; then
    echo "Failed to stop netCAS Thread"
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

# Clean up PMEM device and remove partition if it exists
if [ -e /dev/pmem0 ]; then
    echo "Cleaning PMEM device..."
    
    echo "Cleaning PMEM device metadata..."
    dd if=/dev/zero of=/dev/pmem0 bs=1M count=10 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "Warning: Failed to clean PMEM device metadata"
    else
        echo "PMEM device cleaned successfully"
    fi
else
    echo "No PMEM device found at /dev/pmem0"
fi

# Remove NVMe metadata
if [ -e /dev/nvme2n1 ]; then
    echo "Cleaning NVMe device metadata..."
    if /home/chanseo/shell/disconnect-nvme.sh; then
        echo "NVMe device is disconnected successfully."
    else
        echo "NVMe disconnection failed."
    fi
else
    echo "No NVMe device found at /dev/nvme4n1"
fi

# Final verification
echo "Verifying status..."
casadm -L

if [ $? -eq 0 ]; then
    echo "Teardown completed successfully!"
else
    echo "Teardown encountered issues. Please check manually."
fi