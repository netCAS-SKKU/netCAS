#!/bin/bash

device="nvme2n1"

# Check if script is run as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (sudo)"
    exit 1
fi

# Verify PMEM device exists
if [ ! -e /dev/pmem0 ]; then
    echo "PMEM device /dev/pmem0 not found"
    exit 1
fi
echo "Found PMEM device at /dev/pmem0"

# Verify NVMe device exists
if [ ! -e /dev/$device ]; then
    echo "NVMe device /dev/$device not found"
    exit 1
fi
echo "Found NVMe device at /dev/$device"

# Stop existing cache instance if any
echo "Stopping any existing cache instance..."
casadm -S -i 1 2>/dev/null

# Create new cache instance using PMEM
echo "Creating new cache instance with PMEM..."
if ! casadm -S -i 1 -d /dev/disk/by-id/pmem-9b05c62b-8b4a-4c2a-aec3-6f1b895fa861 -c wo -x 64 --force; then
    echo "Failed to create cache instance"
    exit 1
fi

# Add NVMe as core device
echo "Adding NVMe as core device..."
if ! casadm -A -i 1 -j 1 -d /dev/disk/by-id/nvme-Linux_866827229c7923fb; then
    echo "Failed to add core device"
    casadm -S -i 1  # Stop cache instance on failure
    exit 1
fi

# # Start netCAS Thread
# echo "Starting netCAS Thread..."
# if ! sudo casadm -M -i 1 -j 1; then
#     echo "Failed to start netCAS Thread"
#     exit 1
# fi

# # Changing cache mode to mfcwt
# echo "Changing cache mode to mfcwt..."
# if ! sudo casadm -Q -i 1 -c mfwa --flush-cache yes; then
#     echo "Failed to change cache mode"
#     exit 1
# fi


# Verify setup
echo "Verifying setup..."
casadm -L

echo "Setup completed successfully!" 