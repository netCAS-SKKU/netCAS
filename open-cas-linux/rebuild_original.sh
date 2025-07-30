#!/bin/bash

echo "sudo rmmod cas_cache..."
sudo rmmod cas_cache
sudo rmmod cas_disk

echo "Cleaning previous build..."
sudo make clean

echo "Building with DEBUG=1..."
sudo make DEBUG=1 -j 30

echo "Installing with DEBUG=1..."
sudo make install DEBUG=1 -j 30

echo "sudo modprobe cas_cache..."
sudo modprobe cas_cache
sudo modprobe cas_disk

echo "Build process completed!"