#!/bin/bash
Q="/c/Program Files/qemu/qemu-system-x86_64.exe"
rm -f /tmp/serial.log
"$Q" -accel whpx -kernel build/cache/vmlinuz-virt -initrd build/dev.cpio.gz \
  -append "console=ttyS0 rdinit=/init loglevel=6" \
  -m 1024 -vga std -drive file=build/disk.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -display none -serial file:build/serial.log -monitor stdio -no-reboot
