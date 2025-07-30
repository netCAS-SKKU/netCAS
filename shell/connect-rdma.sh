#!/bin/bash

# # Load NVMe TCP and NVMe Fabrics modules
sudo modprobe nvmet-rdma
sudo modprobe nvme-fabrics

# Connect to NVMe targets
for target in two; do
    sudo nvme connect -t rdma -a 10.0.0.1 -s 4420 -n $target
done

sudo nvme list
