#!/bin/bash
# Script to set up RDMA on a network interface
# Run this script with sudo privileges

# Set the interface to use for RDMA
INTERFACE="ens7"

echo "Setting up RDMA for interface $INTERFACE"

# Check if RDMA modules are loaded
if ! lsmod | grep -q rdma_cm; then
    echo "Loading RDMA core modules..."
    modprobe rdma_cm
    modprobe ib_uverbs
    modprobe rdma_ucm
    modprobe ib_ucm
    modprobe ib_umad
    modprobe ib_ipoib
fi

# Check for Mellanox hardware (common for RDMA)
if lspci | grep -i mellanox; then
    echo "Mellanox hardware detected, loading Mellanox modules..."
    modprobe mlx4_core
    modprobe mlx4_ib
    modprobe mlx5_core
    modprobe mlx5_ib
fi

# Check for Intel hardware
if lspci | grep -i intel; then
    echo "Intel hardware detected, loading Intel modules..."
    modprobe irdma
fi

# Enable IPoIB (IP over InfiniBand) on the interface
if ip link show $INTERFACE &>/dev/null; then
    echo "Configuring $INTERFACE for RDMA..."
    # Set interface up if not already
    ip link set $INTERFACE up
    
    # Check if RDMA is supported on this interface
    if [ -d "/sys/class/infiniband" ]; then
        echo "RDMA subsystem is available"
        # List available RDMA devices
        echo "Available RDMA devices:"
        ls /sys/class/infiniband/
    else
        echo "Warning: RDMA subsystem not found. Make sure your hardware supports RDMA."
    fi
else
    echo "Error: Interface $INTERFACE not found"
    echo "Available interfaces:"
    ip link show | grep -E '^[0-9]+:' | cut -d: -f2
    exit 1
fi

# Install RDMA tools if not already installed
if ! command -v ibv_devices &>/dev/null; then
    echo "Installing RDMA tools..."
    apt-get update
    apt-get install -y rdma-core perftest infiniband-diags
fi

# Display RDMA devices if available
if command -v ibv_devices &>/dev/null; then
    echo "RDMA devices:"
    ibv_devices
    
    echo "RDMA device details:"
    ibv_devinfo
fi

echo "RDMA setup completed for $INTERFACE"
echo "To verify RDMA functionality, you can run performance tests with:"
echo "  ib_send_bw    # For bandwidth test"
echo "  ib_send_lat   # For latency test" 