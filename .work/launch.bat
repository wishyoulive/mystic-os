@echo off
cd /d C:\Users\saket\dev\mystic
"C:\Program Files\qemu\qemu-system-x86_64.exe" ^
  -accel whpx -kernel build\cache\vmlinuz-virt -initrd build\initramfs.cpio.gz ^
  -append "console=ttyS0 rdinit=/init loglevel=3" ^
  -m 1024 -vga std -drive file=build\disk.img,format=raw,if=virtio ^
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 ^
  -rtc base=localtime -no-reboot
