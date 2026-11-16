# MYSTIC OS

A beautiful, small, secure desktop OS for old PCs. It runs Linux apps natively
and Windows apps through Wine.

Not a Linux skin. Not a distro remix. The desktop is ours: our compositor, our
theme, our shell, our security model.

## What works today

`image.sh` builds a **3.7 MB** bootable root filesystem from Alpine's kernel +
our own userspace, and boots it in QEMU to a shell. That is the floor
everything else stands on: a real kernel, a real rootfs we assemble, a real
boot.

```
stage/        our userspace - the only files that are "us"
  init          PID 1: mounts, banner, hands over to the shell
tools/
  mkimage.py    builds the rootfs image (cpio) straight from tarballs
  ppm2png.py    turns QEMU screenshots into PNG
image.sh        download kernel -> pack rootfs -> boot
boot/boot.asm   stage 1: our own 512-byte boot sector (kept as a lesson)
build.sh        builds that boot sector into a floppy image
```

## Build and run

```bash
./image.sh          # build + boot in a window
./image.sh shell    # build + boot with a serial console
./image.sh shot 10  # build, boot headless, screenshot after 10s
```

## Design targets (the things we will be judged on)

| | Target |
|---|---|
| Idle RAM | under 128 MB with the desktop running |
| Boot to desktop | under 2 seconds on 2010-era hardware |
| CPU baseline | x86-64 without SSE4.2 / POPCNT; no TPM, no Secure Boot |
| Apps | Linux native, Windows via Wine |
| Security | read-only root, every app sandboxed, per-app permissions you can see |

## Rules this project keeps

1. Nothing is "done" until it has been built and booted.
2. No file grows by copy-paste. Duplication gets deleted.
3. One idea at a time. The next idea starts when the last one boots.
4. Claim nothing that was not measured here.
