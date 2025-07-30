#!/bin/bash

cd ../shell
sudo ./teardown_opencas_pmem.sh
cd ../open-cas-linux

echo "sudo rmmod cas_cache..."
sudo rmmod cas_cache

echo "Cleaning previous build..."
sudo make clean

echo "Building with DEBUG=1..."
sudo make DEBUG=1 -j 30

echo "Installing with DEBUG=1..."
sudo make install DEBUG=1 -j 30

echo "sudo modprobe cas_cache..."
sudo modprobe cas_cache

echo "Build process completed!"

cd ../shell
sudo ./connect-rdma.sh
sudo ./setup_opencas_pmem.sh