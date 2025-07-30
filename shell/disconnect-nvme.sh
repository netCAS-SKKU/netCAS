#!/bin/bash

# Disconnect from NVMe targets
for target in one, two; do
    sudo nvme disconnect -n $target
done
