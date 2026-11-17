#!/bin/bash
Q="/c/Program Files/qemu/qemu-system-x86_64.exe"
"$Q" -accel whpx -kernel build/cache/vmlinuz-virt -initrd build/dev.cpio.gz \
  -append "console=ttyS0 rdinit=/init loglevel=4" \
  -m 512 -vga std -drive file=build/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -display none -monitor stdio -no-reboot
