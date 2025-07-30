#!/bin/bash

# Load NVMe TCP and NVMe Fabrics modules
sudo modprobe nvme-tcp

# Connect to NVMe targets
for target in nvme-subsystem; do
    sudo nvme connect -t tcp -a 10.0.0.6 -s 4420 -n nvme-subsystem
done

sudo nvme list