# Hardware: what works, on which kernel

Mystic OS ships one kernel by default and one optional bigger one. They are
not the same hardware story, and the difference is not a setting: the default
kernel has no camera driver in it at all.

| | `linux-virt` (default) | `linux-lts` (`./image.sh --hw`) |
|---|---|---|
| package | linux-virt-6.18.63-r0.apk, **44,585,823 bytes** | linux-lts-6.18.63-r0.apk, **146,407,326 bytes** |
| virtio disk / net | yes | yes |
| PS/2 keyboard + mouse | yes (`evdev`, `psmouse`) | yes |
| framebuffer / DRM | yes (`bochs`, `virtio-gpu`, `cirrus-qemu`, `qxl`, `simpledrm`, `vkms`) | yes, plus the DRM tree lts carries |
| HDA audio playback | yes (`snd-hda-intel`, `snd-hda-codec`, `snd-pcm`, `snd-timer`, `virtio_snd`) | yes |
| HDA capture (on-board analog mic) | the codec decides; init reports any `/dev/snd/pcmC*D*c` that appears | same |
| USB host + HID | yes — `usbcore`, `xhci-hcd/xhci-pci`, `ehci-hcd/ehci-pci`, `ohci-*`, `uhci-hcd`, `usbhid`, `hid`, `hid-generic` | yes |
| **USB webcam (UVC)** | **NO — there is no `kernel/drivers/media` in this package** | yes — `uvcvideo` |
| **USB microphone** | **NO — there is no `kernel/sound/usb` in this package** | yes — `snd-usb-audio` |
| Wi-Fi | no | yes — `cfg80211`, `mac80211`, 171 driver modules |
| amdgpu | no | module present (`amdgpu.ko`, 30,253,983 bytes) but it needs firmware this image does not carry — **untested** |

## The short version

**The default image cannot support a webcam, and no userspace change can fix
that.** `linux-virt` does not ship `videodev`, `uvcvideo`, `videobuf2-*` or
`snd-usb-audio` — the files do not exist in the package, so there is nothing
to load (verified: no `kernel/drivers/media` directory and no `snd-usb-*` file
anywhere in `linux-virt-6.18.63-r0`). `./image.sh --hw` is the only path that
changes this; it swaps in `linux-lts`, which carries the whole `drivers/media`
tree and `sound/usb`.

An earlier assumption was that virt also lacks the USB drivers. That part is
wrong and worth correcting: virt **does** ship `usbcore`, the xHCI/EHCI/OHCI/
UHCI host controllers and `usbhid`, so a USB keyboard or mouse works there.
What it lacks is exactly the two trees a camera and a USB microphone need.

## What the hardware image costs

Measured by `tools/fetch-kernel.sh lts` and `tools/mkimage.py` on this tree:

| | virt | lts |
|---|---|---|
| module files in the image | **222** (`32,477,802` bytes) | **1,909** (`303,741,443` bytes) |
| of which `kernel/drivers/media` | 0 | **485** (`35,576,035` bytes) |
| of which `kernel/drivers/media/usb` | 0 | 103 |
| of which `kernel/sound/usb` | 0 | 13 |
| of which `kernel/drivers/net/wireless` | 0 | 171 (`53,501,813` bytes) |
| initramfs | **15.9 MB** (`47.9 MB` raw) | **84.5 MB** (`319.5 MB` raw) |
| RAM to boot | 256 MB (default) | **1024 MB** (default for `--hw`) |

`./image.sh --hw` prints those numbers before it boots and warns about them.
The module payload is 9.4x the default one; that is the honest price of having
the drivers at all.

## The webcam and the microphone, module by module

These are not guesses: they are the transitive closure of the drivers over the
package's own `modules.dep`, printed by `tools/fetch-kernel.sh` and copied into
the image. Nothing is named that is not in the package.

Camera (`uvcvideo`) — **10 modules, 2,746,662 bytes**:

| module | bytes |
|---|---|
| `kernel/drivers/media/usb/uvc/uvcvideo.ko` | 403,119 |
| `kernel/drivers/media/common/uvc.ko` | 12,623 |
| `kernel/drivers/media/common/videobuf2/videobuf2-common.ko` | 176,183 |
| `kernel/drivers/media/common/videobuf2/videobuf2-v4l2.ko` | 106,839 |
| `kernel/drivers/media/common/videobuf2/videobuf2-vmalloc.ko` | 44,079 |
| `kernel/drivers/media/common/videobuf2/videobuf2-memops.ko` | 18,999 |
| `kernel/drivers/media/v4l2-core/videodev.ko` | 817,287 |
| `kernel/drivers/media/mc/mc.ko` | 191,727 |
| `kernel/drivers/usb/core/usbcore.ko` | 936,303 |
| `kernel/drivers/usb/common/usb-common.ko` | 39,503 |

Microphone (`snd-usb-audio`) — **12 modules, 3,576,652 bytes**:
`snd-usb-audio` (1,161,311), `snd-usbmidi-lib` (111,551), `snd-hwdep`,
`snd-rawmidi`, `snd-seq-device`, `mc`, `usbcore`, `usb-common`, `snd-pcm`
(430,335), `snd-timer`, `snd`, `soundcore`.

USB HID — 5 modules, 1,635,539 bytes. Wi-Fi stack core — 4 modules,
6,386,996 bytes (`cfg80211` 3,151,431, `mac80211` 3,145,135, `rfkill`,
`libarc4`).

Two names people expect and will **not** find as modules in 6.18:

* `v4l2-common` is not a module any more; its code is linked into
  `videodev.ko`. Asking modprobe for `v4l2-common` fails, which is fine —
  `videodev` is what uvcvideo actually depends on.
* there is no `uvc.ko` under `drivers/media/usb/uvc/`. The shared UVC helper is
  `kernel/drivers/media/common/uvc.ko` (12,623 bytes), and it is already in the
  closure above.

## Firmware (Wi-Fi, amdgpu)

Modules are not enough for most real Wi-Fi cards or for amdgpu: those drivers
want firmware blobs at run time. The image does **not** carry any firmware
package. Real package sizes from the Alpine v3.24 index
(`linux-firmware` 20260519-r0), as `S:` (download) and `I:` (installed):

| firmware package | download | installed |
|---|---|---|
| `linux-firmware-amdgpu` | 27,485,693 | 27,429,919 |
| `linux-firmware-intel` (contains iwlwifi) | 121,770,217 | 121,764,870 |
| `linux-firmware-mediatek` | 29,607,011 | 29,604,874 |
| `linux-firmware-ath11k` | 23,597,896 | 23,582,271 |
| `linux-firmware-rtw89` | 5,184,486 | 5,206,984 |
| `linux-firmware-rtlwifi` | 603,096 | 595,629 |
| `linux-firmware-rtw88` | 409,052 | 405,229 |

**amdgpu**: the module is in the lts image (`amdgpu.ko`, 30,253,983 bytes; the
whole `drm/amd` tree is 30,274,286 bytes in two modules), but without
`linux-firmware-amdgpu` (27,485,693 bytes) it cannot bind to a card, and no
amdgpu hardware was available to test it on. It is shipping, not working:
treat it as untested.

**Wi-Fi**: the stacks and 171 drivers are in the image (`iwlwifi`, `iwlegacy`,
`rtw88`, `rtw89`, `rtl8xxxu`, `rtl8187`, `rt2800usb`, `mt76*`, `ath9k_htc`,
`ath10k_usb`, `ath11k` (PCI only in this tree), `brcmfmac`, `p54usb`,
`zd1211rw`, `libertas`, `rsi_usb`, `cw1200`, …). Most of them need at least one
firmware package from the table, which is a deliberate omission, not a bug.

## What init does with it

`stage/init` decides from the filesystem, not from a flag:

1. if `/lib/modules/$(uname -r)/kernel/drivers/media` exists, it modprobes
   `usbcore`, the host controllers, `usbhid`, `mc`, `videodev`, `videobuf2-*`,
   `uvcvideo`, and the `snd-*` + `snd-usb-audio` chain;
2. it then prints the truth: `hw: kernel = lts (…)` or `hw: kernel = virt (…)`,
   the USB bus/device count from sysfs, every `/dev/video*` with the name the
   driver registered, and every `/dev/snd/pcmC*D*c` capture node;
3. it `chmod 0666`s the camera nodes and the raw `/dev/bus/usb/*/*` devices for
   the same reason the playback PCM nodes are chmodded: devtmpfs leaves them
   `0600 root:root`, there is no udev here, and the app jail runs as uid 1000;
   capture PCM nodes are **reported, not opened** — opening a microphone to
   every jailed app is a privacy decision, not a build flag, and playback is
   the only class that is opened automatically today;
4. when nothing appears it says why, and the "why" is chosen from what it can
   see — no drivers/media in this kernel / uvcvideo did not load (with
   modprobe's own error line) / no USB bus / bus present but no UVC camera or
   USB audio device attached.

`./image.sh --hw probe` prints the same facts inside the probe block.

## How to try it

```sh
./image.sh --hw probe                 # boot the lts image, print facts, power off
./image.sh --hw                       # boot the desktop on linux-lts (needs ~1 GB)
MYSTIC_USB_HOST=1234:5678 ./image.sh --hw    # pass a host webcam through to QEMU
```

`--hw` also adds `-device qemu-xhci` (the default QEMU command line has no USB
controller, so no USB device could ever appear). `MYSTIC_USB_HOST` is
`vendor:product` from `lsusb` on the host; it is wired up but **was not
exercised for this task** — no QEMU boot was run, and there is no webcam on the
build machine. What *was* verified: the lts package really contains the modules
above, the closure is complete against `modules.dep`, both images build, and
`stage/init`'s new code paths were exercised with fake `/dev`, `/sys`, `/proc`
trees (`.work/drivers/test-hw-init.sh`).

## The next job: an app that actually captures

No camera or recording app exists yet, on purpose. What such an app needs from
the kernel, once the `--hw` image is booted, is the V4L2 streaming ioctl
sequence — in this order, on `/dev/video0`:

| ioctl | struct | why |
|---|---|---|
| `VIDIOC_QUERYCAP` | `struct v4l2_capability` | driver/card name, `V4L2_CAP_VIDEO_CAPTURE`, `V4L2_CAP_STREAMING` |
| `VIDIOC_ENUM_FMT` | `struct v4l2_fmtdesc` | the pixel formats it really offers (MJPEG, YUYV, NV12 …) |
| `VIDIOC_S_FMT` | `struct v4l2_format` | the size + format to use |
| `VIDIOC_REQBUFS` | `struct v4l2_requestbuffers` | N buffers, `V4L2_MEMORY_MMAP` (or `DMABUF`) |
| `VIDIOC_QUERYBUF` + `mmap` | `struct v4l2_buffer` | map each buffer into the app |
| `VIDIOC_QBUF` | `struct v4l2_buffer` | hand a buffer to the driver |
| `VIDIOC_STREAMON` | `enum v4l2_buf_type` | start streaming |
| `VIDIOC_DQBUF` | `struct v4l2_buffer` | the frame that is ready (blocks; use `poll`) |
| `VIDIOC_STREAMOFF` / `munmap` | | stop cleanly |

The constants live in `<linux/videodev2.h>`; musl has no such header, so the
structs and ioctl numbers have to be written out in the app the same way
`src/audio.c` writes out the ALSA PCM UAPI. Microphone capture is the other
half: `/dev/snd/pcmC*D*c` with `SNDRV_PCM_IOCTL_HW_PARAMS` for
`SNDRV_PCM_ACCESS_RW_INTERLEAVED`, then `SNDRV_PCM_IOCTL_READI_FRAMES` in a
loop — and, before that, a decision about the node mode: capture nodes are
left root-only on purpose (see above), so the recorder is the job that has to
open them for uid 1000 the way playback is opened.

## Limits, stated plainly

* Ubuntu/Windows webcams that are UVC class should bind to `uvcvideo`; vendor
  webcams that need `gspca_*` also have their drivers in the image (103 modules
  under `drivers/media/usb`), but no `gspca` device was tested.
* The lts image still has no firmware packages, so most Wi-Fi and all amdgpu
  devices will probe and fail.
* No camera hardware was available. Everything above about what *binds* is
  derived from what is in the package; everything about what *works* on real
  silicon is untested and labelled as such.
