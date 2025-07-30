# netCAS

netCAS is a network-aware caching system based on OpenCAS (Open Cache Acceleration Software). This repository contains modifications to the OpenCAS Linux kernel module to support network-aware caching functionality.

## Overview

netCAS extends the OpenCAS caching framework to provide network-aware caching capabilities, allowing for intelligent cache placement and management based on network conditions and topology.

## Repository Structure

- `open-cas-linux/` - Modified OpenCAS Linux kernel module with netCAS enhancements
- `shell/` - Shell scripts for setup, teardown, and management of netCAS components
- `test/` - Test files and utilities

## Key Features

- Network-aware cache placement
- Dynamic cache mode switching based on network conditions
- RDMA and NVMe-oF support
- PMEM (Persistent Memory) integration
- Advanced split ratio optimization

## Getting Started

### Prerequisites

- Linux kernel headers
- Build tools (make, gcc)
- RDMA drivers (for RDMA functionality)
- NVMe drivers (for NVMe-oF functionality)

### Building

```bash
cd open-cas-linux
make
```

### Installation

```bash
# Load the kernel module
sudo insmod modules/cas_cache/cas_cache.ko
sudo insmod modules/cas_disk/cas_disk.ko

# Install management tools
sudo make install
```

## Usage

See the `shell/` directory for setup and management scripts:

- `setup_opencas.sh` - Basic OpenCAS setup
- `setup_opencas_pmem.sh` - Setup with PMEM support
- `setup_rdma.sh` - Setup with RDMA support
- `connect-nvme.sh` - Connect to NVMe-oF targets
- `connect-rdma.sh` - Connect to RDMA targets

## License

This project is based on [OpenCAS](https://github.com/Open-CAS/open-cas-linux), which is licensed under the BSD 3-Clause License. See the LICENSE file in the `open-cas-linux/` directory for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## Contact

For questions and contributions, please contact the netCAS development team.

## Acknowledgments

This project is based on the OpenCAS project by Intel. We thank the OpenCAS community for their excellent work on the caching framework.
