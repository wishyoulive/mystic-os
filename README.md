# MYSTIC OS

An operating system written from scratch. It boots on real hardware.

**Status: stage 1.** The boot sector fits in 512 bytes, is loaded by the BIOS at
`0x7C00`, switches the screen to text mode and prints a banner. Then it halts.

## Build

```bash
./build.sh
```

Produces `build/mystic.img` — a 1.44 MB bootable disk image.

## Run

No emulator is assumed. To see it boot:

- **in QEMU** (once installed): `qemu-system-x86_64 -drive format=raw,file=build/mystic.img`
- **on a real machine**: write `build/mystic.img` to a USB stick and boot it

## Layout

```
boot/boot.asm    the 512-byte boot sector: where the machine wakes up
build.sh         assemble + pack into a disk image
build/           output (never committed)
```

## Rules this project keeps

1. Nothing is "done" until it has been built and booted.
2. No file grows by copy-paste. If something is duplicated, it gets deleted.
3. One idea at a time. The next idea is only started when the last one boots.
