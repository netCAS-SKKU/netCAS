sudo rm -rf /home/chanseo/open-cas-linux-split
sudo modprobe -r cas_cache
sudo modprobe -r cas_disk
sudo rm -rf /lib/modules/$(uname -r)/extra/cas_cache.ko
sudo rm -rf /lib/modules/$(uname -r)/extra/cas_disk.ko
sudo rm -f /usr/sbin/casadm
sudo rm -f /usr/sbin/casctl
sudo rm -rf /usr/include/cas
