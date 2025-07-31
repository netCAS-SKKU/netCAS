#!/bin/bash

set -e  # Exit on any error

echo "Starting Open CAS Linux rebuild process..."

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check if we're in the right directory
if [ ! -f "Makefile" ]; then
    echo "Error: Makefile not found. Please run this script from the open-cas-linux directory."
    exit 1
fi

# Check if required commands exist
if ! command_exists make; then
    echo "Error: make command not found"
    exit 1
fi

if ! command_exists modprobe; then
    echo "Error: modprobe command not found"
    exit 1
fi

# Teardown existing setup
echo "Tearing down existing Open CAS setup..."
cd ../shell
if [ -f "teardown_opencas_pmem.sh" ]; then
    sudo ./teardown_opencas_pmem.sh || echo "Warning: Teardown had issues, continuing..."
else
    echo "Warning: teardown_opencas_pmem.sh not found, skipping teardown"
fi
cd ../open-cas-linux

# Remove existing modules
echo "Removing existing kernel modules..."
sudo rmmod cas_cache 2>/dev/null || echo "cas_cache module not loaded"
sudo rmmod cas_disk 2>/dev/null || echo "cas_disk module not loaded"

# Clean previous build
echo "Cleaning previous build..."
sudo make clean

# Build with DEBUG=1
echo "Building with DEBUG=1..."
sudo make DEBUG=1 -j $(nproc)

# Install
echo "Installing with DEBUG=1..."
sudo make install DEBUG=1 -j $(nproc)

# Load modules
echo "Loading kernel modules..."
sudo modprobe cas_disk
sudo modprobe cas_cache

echo "Build process completed successfully!"

# Setup Open CAS
echo "Setting up Open CAS..."
cd ../shell

# Connect RDMA if script exists
if [ -f "connect-rdma.sh" ]; then
    sudo ./connect-rdma.sh || echo "Warning: RDMA connection had issues"
else
    echo "Warning: connect-rdma.sh not found, skipping RDMA setup"
fi

# Setup Open CAS PMEM if script exists
if [ -f "setup_opencas_pmem.sh" ]; then
    sudo ./setup_opencas_pmem.sh || echo "Warning: Open CAS PMEM setup had issues"
else
    echo "Warning: setup_opencas_pmem.sh not found, skipping Open CAS setup"
fi

echo "Rebuild process completed!"