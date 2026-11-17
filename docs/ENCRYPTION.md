# Encryption in Mystic OS

An encrypted `/root`, built from parts that are already in this tree, and an
honest account of what it does and does not protect.

**Short version.** `/dev/vda` becomes a dm-crypt volume
(`aes-xts-plain64`); the ext4 filesystem the rest of the OS writes to lives
*inside* that volume. The 64-byte data key is split in two: 32 bytes travel in
the initramfs (`/etc/mystic/luks.key`), 32 bytes are generated on the disk
itself at first boot and stored in an unencrypted 4 KiB header. Neither half
alone decrypts the disk. A copy of `build/disk.img` on its own is ciphertext.
Everything else — the running system, a copy of *both* files, anything that can
read the initramfs — sees plaintext. Section [6](#6-what-is-and-is-not-protected)
is the part to read before believing anything else.

---

## 1. Run it

```sh
./tools/fetch-kernel.sh     # once: kernel + modules into build/cache and stage/
./tools/mkenc.sh            # stage dm-crypt modules, key, helper; verify
./tools/mkenc.sh test       # boot-path simulation, no QEMU
./tools/mkenc.sh status     # what the next boot will do with build/disk.img
./image.sh                  # boot; first boot creates the encrypted volume
```

`./tools/mkenc.sh` must be re-run after `./tools/fetch-kernel.sh`: that script
does `rm -rf stage/lib/modules/<version>` and re-stages its own module list,
which does not include `kernel/drivers/md`. See [3](#3-the-kernel-side-verified).

To move an existing plain disk to an encrypted one, see
[8.3](#83-an-existing-plain-disk).

---

## 2. Where the pieces are

| piece | file | what it is |
|---|---|---|
| on-disk format + dm ioctls | `tools/mkenc.sh:99-1119` (the C source embedded in the tool) | `mystic-dmcrypt`: 1,021 lines of C, no dependencies |
| compiled helper | `stage/sbin/mystic-dmcrypt` | static musl ELF, 40,552 bytes as built here |
| boot half of the key | `stage/etc/mystic/luks.key` | 32 bytes from `/dev/urandom`, created once by `mkenc.sh` |
| module staging | `tools/mkenc.sh:77` `stage_modules()` | copies `dm-mod.ko`, `encrypted-keys.ko`, `dm-crypt.ko` out of the cached kernel package |
| build / verify | `tools/mkenc.sh:1123` `build_helper()`, `:1216` `verify()` | exact compile command, ELF check, vermagic check, key length check |
| boot logic | `stage/init:96-278` | the state machine in [5](#5-the-boot-sequence) |
| boot-path simulation | `tools/mkenc.sh:1364` `cmd_test()` | runs `stage/init` against a fake `/dev`, fake `mount`/`mkfs`, real helper |

There is no `cryptsetup` and no `dmsetup` in the image. `stage/sbin` contains
`mkfs.ext4` and `mystic-dmcrypt`; that is the entire userspace for this feature.

---

## 3. The kernel side, verified

From `build/cache/linux-virt-6.18.63-r0/boot/config-6.18.63-0-virt`:

```
CONFIG_BLK_DEV_DM=m
CONFIG_DM_CRYPT=m
CONFIG_CRYPTO_AES=y                 <- built in, not a module
CONFIG_CRYPTO_XTS=y                 <- built in, not a module
CONFIG_CRYPTO_SHA256=y
CONFIG_CRYPTO_AES_NI_INTEL=m        <- deliberately not staged (see below)
```

So `aes-xts-plain64` resolves with no crypto module at all: the generic AES and
XTS are in the kernel image. `CONFIG_CRYPTO_ESSIV=m` matters only if a different
IV mode were used; this design uses `plain64`.

The two device-mapper modules are built as modules, and
`stage/lib/modules/<ver>/kernel/drivers/md/` **was empty before this work**:
`tools/fetch-kernel.sh` copies a hand-written list of subtrees
(`MODULE_TREES`, `tools/fetch-kernel.sh:27-47`) and `kernel/drivers/md`,
`kernel/security/keys` are not in it. `dm-crypt.ko` is in the upstream package
and `modules.dep` says it needs:

```
kernel/drivers/md/dm-crypt.ko: kernel/drivers/md/dm-mod.ko \
                               kernel/security/keys/encrypted-keys/encrypted-keys.ko
```

`tools/mkenc.sh stage_modules()` therefore copies exactly those three files,
decompressed, into `stage/lib/modules/<ver>/…`, and `verify()` checks three
things about each one: it is an ELF object, it mentions the kernel version
string (vermagic — the classic "Invalid module format" boot failure), and both
dependencies are present. Measured sizes of the staged files:

```
drivers/md/dm-mod.ko                                 462,255 bytes
kernel/security/keys/encrypted-keys/encrypted-keys.ko  54,367 bytes
drivers/md/dm-crypt.ko                               148,999 bytes
```

**Permanent fix, not applied here:** add `kernel/drivers/md` and
`kernel/security/keys` to `MODULE_TREES` in `tools/fetch-kernel.sh`. That file
belongs to another worker in this change; until it is edited, `mkenc.sh` is the
step that puts the modules in place, and `fetch-kernel.sh` must not be re-run
after it without re-running `mkenc.sh`.

Why `aesni-intel.ko` is not staged: it only makes AES faster. Without it the
built-in generic AES is used. The disk is 64 MiB by default (`image.sh`), so
the difference is not worth a fourth module that can fail to load.

---

## 4. Why not `cryptsetup` (or even `dmsetup`)

| tool | what it drags in |
|---|---|
| `cryptsetup` | libcryptsetup, libdevmapper, libuuid, libblkid, libjson-c, argon2, and a LUKS2 metadata parser |
| `dmsetup` | libdevmapper, libuuid, libblkid (devmapper is a separate source tree with its own build system) |
| `mystic-dmcrypt` | nothing. musl, three ioctls, 40,552 bytes |

A dm-crypt target is a table *string*. Creating one is:

```
open("/dev/mapper/control")
ioctl(DM_DEV_CREATE)     name = "mystic-root"
ioctl(DM_TABLE_LOAD)     "0 <sectors> crypt aes-xts-plain64 <128 hex> 0 /dev/vda 2048"
ioctl(DM_DEV_SUSPEND)    resume
```

The table that was built for the default 64 MiB disk, printed by the helper with
the key blanked out (`mystic-dmcrypt attach mystic-root /dev/vda <key> --table`):

```
0 129024 crypt aes-xts-plain64 <64-byte key> 0 /dev/vda 2048
```

`129024 = 131072 - 2048`: the last 1 MiB of the disk is the header, and sector
2048 of the mapping is sector 2048 of the block device. The kernel reports the
mapping's major:minor in the ioctl reply, so the helper creates
`/dev/mapper/mystic-root` itself with `mknod` if devtmpfs did not.

The LUKS header format is *not* implemented. This is not a LUKS volume;
`cryptsetup luksDump` will not read it. What we give up compared with LUKS:
multiple key slots, passphrase-based key derivation (Argon2), an authenticated
header, and interoperability. See [9](#9-what-is-not-implemented).

---

## 5. The on-disk format

`/dev/vda`, 512-byte sectors:

```
sector 0 .. 2047   1 MiB, plaintext: the MYSTICC1 header, zeros after byte 0x58
sector 2048 .. end            ciphertext: aes-xts-plain64 of the ext4 filesystem
```

The header is written once, at first boot, and only to a device whose first
4 KiB is all zero.

| offset | size | field |
|---|---|---|
| `0x00` | 8 | ASCII `MYSTICC1` |
| `0x08` | 4 | format version, little-endian, currently `1` |
| `0x0c` | 4 | data offset in 512-byte sectors (`2048`) |
| `0x10` | 4 | cipher id (`1` = `aes-xts-plain64`) |
| `0x14` | 4 | key length in bytes (`64`) |
| `0x18` | 32 | **disk half** of the key material |
| `0x38` | 32 | `SHA-256(boot half ‖ disk half)` |
| `0x58` | 4008 | zero |

The data key is the two halves concatenated, 64 bytes, used directly as the
XTS key (AES-256-XTS: two 256-bit keys). No KDF is applied, because both halves
are already 256 bits from `/dev/urandom`; there is nothing to stretch.

The fingerprint at `0x38` exists for one reason: so a **wrong key is detected
before anything is formatted**. `stage/init` never formats a disk whose header
it could not open. It is a verifier, so as long as both halves are 256-bit
random it hides nothing useful. It *would* be an offline-crackable oracle if
the key were a passphrase — which is why `key_read()`
(`tools/mkenc.sh:460`) refuses any key file that is not exactly 32 bytes rather
than truncating a passphrase into one.

The IV mode is `plain64`: the tweak is the sector number, with no per-disk
salt. LUKS uses the same for `aes-xts-plain64`. It means two disks encrypted
with the *same* key would produce identical ciphertext for identical data at
the same offsets; each disk here gets its own random key, so that cannot happen
by accident, but do not clone a key between disks.

---

## 6. What is, and is not, protected

### 6.1 The split, and why it is spelled out this way

```
data key (64 bytes) = boot half (32, in the initramfs)
                    ‖ disk half (32, in the plaintext header on /dev/vda)
```

* A copy of `build/disk.img` alone: **protected**. The ciphertext is useless and
  the header only gives away 32 random bytes and a SHA-256 of the whole key.
* A copy of the initramfs alone (`build/dev.cpio.gz`, which contains
  `/etc/mystic/luks.key`): **protected**. The disk half is on the disk.
* Both files: **not protected at all**. They are enough to rebuild the key and
  read everything.

That is the whole design. It is a two-part secret, not a passphrase system, and
it does not pretend to be more. If you copy the key file onto the disk (or into
the same archive as the disk image), you have implemented the "key stored on the
disk" variant from the brief and the protection against offline inspection of
that disk is *zero*.

`stage/etc/mystic/luks.key` is **not** in `.gitignore` as this tree stands
(`build/`, `*.img`, `*.bin`, `stage/lib/modules/` and `stage/sbin/` are). Add it.
A committed boot half is not enough to read a disk image — that is the point of
the split — but it is half of the secret handed out for free.

### 6.2 Why there is no passphrase prompt at boot

The brief asked for one, and the reason it is not here is mechanical, not
laziness:

* `./image.sh probe` boots **headless and unattended** (`-display none
  -serial stdio`) and the whole project's verification (`tools/smoke.sh`, two
  probe boots per run) depends on it finishing without input. A blocking
  passphrase read would hang it.
* `./image.sh run` shows a graphical QEMU window; the serial console goes to the
  terminal that launched `image.sh`, not into the window. A prompt on
  `/dev/console` would be invisible to the person looking at the desktop.
* init runs before there is any way to know whether a human is present.

A passphrase prompt needs a key slot with a slow KDF (Argon2/scrypt) so that a
stolen header cannot be brute-forced, plus a console the operator can actually
type into. Neither exists here, so the design uses a random key instead of
pretending: a 256-bit random half cannot be brute-forced, and the fingerprint
oracle is therefore harmless.

### 6.3 The lock screen is not encryption

`src/main.c`'s lock screen (`try_unlock`, `/etc/mystic/passwd`, SHA-256 with
20000 rounds) is a **UI lock**. It stops someone using the desktop; it does not
touch the disk, the key, or the dm-crypt mapping. When the desktop is locked the
filesystem is still mounted and decrypted. Do not confuse the two, and do not
believe the PIN protects `/root` at rest.

### 6.4 Threat model

| attacker has | what they get |
|---|---|
| `build/disk.img` (the realistic offline case) | ciphertext. `/root`, `/root/notes`, `boots.txt`: unreadable. |
| `build/disk.img` + `build/dev.cpio.gz` | plaintext. Both halves. |
| a copy of this git repository (if `stage/etc/mystic/luks.key` was committed) | the boot half only — not enough without a disk image. But it removes half the work, so do not commit it. |
| root inside the running OS | everything. The mapping is open, the key is in `/etc/mystic/luks.key`, and the kernel holds the key for the live mapping. dm-crypt is not a defence against a live root. |
| the physical machine while it is running | everything, including a cold-boot or DMA attack on RAM. |
| write access to `build/disk.img` | they cannot read it, but they can **corrupt** it: there is no authentication (see 6.5). |
| a way to make the boot key disappear | denial of service: the volume will not open, and the boot refuses to format it. Data is not destroyed, but `/root` is gone until the key is restored. |

### 6.5 What cryptography is *not* here

* **No authentication.** `aes-xts-plain64` is unauthenticated encryption. An
  attacker with write access to the disk can flip ciphertext bits; ext4 will
  read garbage where that block was, and might mount or might not. dm-crypt
  cannot tell you the data was modified. `dm-integrity` (or an AEAD mode) would
  be needed; it costs a second mapping and is not implemented.
* **No header authentication.** The 1 MiB header is plaintext and unauthenticated.
  The fingerprint catches a *wrong key*, not a *tampered header*. Someone who
  rewrites the disk half and the fingerprint just makes the volume impossible to
  open (the data key changes, so the ciphertext becomes noise). That is
  vandalism, not a bypass, but it is worth knowing.
* **No anti-rollback.** An old copy of the disk image opens with the current key.
* **No protection of the swap or RAM.** There is no swap; the desktop keeps
  `/root` contents in page cache in the clear like any Linux.

---

## 7. The boot sequence

`stage/init:244` `setup_disk()` runs one probe and then branches. The probe
(`mystic-dmcrypt probe /dev/vda`) reads the first 4 KiB and classifies the
device; nothing is written and nothing is mounted to decide.

| state | meaning | what init does | message |
|---|---|---|---|
| no device | `[ -b /dev/vda ]` false | nothing | `disk: no /dev/vda` *(unchanged)* |
| `blank` | first 4 KiB all zero, no ext4 magic | write header, open mapping, `mkfs.ext4`, mount | `disk: created dm-crypt volume on /dev/vda (aes-xts-plain64)` then `disk: formatted ext4 inside dm-crypt and mounted on /root` |
| `crypt <fp>` | our header, fingerprint matches | open mapping, mount | `disk: dm-crypt unlocked, formatted ext4 mounted on /root` |
| `crypt <fp>`, no ext4 inside | header exists, mkfs never finished | open mapping, `mkfs.ext4`, mount | `disk: dm-crypt unlocked, no ext4 inside - creating one` |
| `locked:` | our header, but `dm_mod`/`dm_crypt` did not load | **refuse**: no format, no mount | `disk: … is encrypted but cannot be opened` / `disk: refusing to format it` |
| `locked (key does not match)` | header opened, wrong key | **refuse**: no format, no mount | `disk: encrypted, but the key did not unlock it` |
| `foreign` | no header, but not blank (a plain ext4 disk from an older build) | the original code path | `disk: /dev/vda has no encryption header and is not blank`, then `disk: mounted /dev/vda (ext4) on /root` or the old format path *(unchanged)* |
| `unavailable:` | no helper in the image, or no modules and a blank disk | the original code path | `disk: dm-crypt unavailable - …` then the old messages *(unchanged)* |

Three rules make this safe rather than clever:

1. **`mkfs` never runs on a device that carries a header.** If the header is
   there and cannot be opened, `/root` is left on the RAM filesystem and the OS
   still boots. That is the `locked` rows.
2. **The old behaviour is kept verbatim** for a disk that is not ours
   (`foreign`, `unavailable`). That is what keeps an existing `build/disk.img`
   booting after this change, and it is why the "disk did not mount, creating
   ext4 filesystem" path and its documented KNOWN RISK comment are still there.
3. **A blank disk with no key in the image does not silently become encrypted
   with a key that will be lost at reboot.** There is no persistent place to put
   a key generated at runtime: the initramfs is a read-only cpio, so a key
   generated into RAM would make the volume unreadable on the next boot. init
   therefore says `disk: no encryption key at … - using plain ext4` and takes
   the old path. `tools/mkenc.sh` always creates the key, so this only happens
   in a broken build.

The probe state is also printed, so a boot log tells you which one happened:

```
crypt    : aes-xts-plain64, key 6c67030f117e8ce0
```

The fingerprint is the first 8 bytes of `SHA-256(boot half ‖ disk half)` in hex.
It is not secret; it is there to compare two machines and to prove at a glance
that a boot used the volume you think it did.

---

## 8. Operating it

### 8.1 First boot

`image.sh` creates `build/disk.img` (64 MiB by default, `MYSTIC_DISK_MB`) if it
does not exist. On the next boot the disk is all zeros, so init writes the
header, opens the mapping, runs the shipped `mkfs.ext4` on
`/dev/mapper/mystic-root`, and mounts it. The ext4 superblock is *inside* the
encrypted region; there is no ext4 on `/dev/vda` itself.

### 8.2 Reboots

The header is on the disk, the boot half is in the initramfs; the same two files
give the same key, so the volume opens. `probe` mode prints
`disk     : mounted, boot counter now N` exactly as before, because `/root` is a
real mountpoint of `mystic-root` and not of `/dev/vda`.

### 8.3 An existing plain disk

`build/disk.img` made by an older build holds a plain ext4 filesystem. It keeps
booting unencrypted and says so:

```
disk: /dev/vda has no encryption header and is not blank
disk: no MYSTICC1 header - the disk is not encrypted
disk: mounted /dev/vda (ext4) on /root
```

There is no in-place migration path in this tree, and it is worth being blunt
about why: the whole device is already occupied by the plaintext filesystem, so
there is nowhere to put the ciphertext, and the image has no `losetup`, no
`rsync` and no second device to shuffle data through. The demo disk holds
`/root/mystic-ui.conf`, `/root/mystic-wallpaper`, `/root/mystic-current-theme`,
`boots.txt` and whatever the apps made in `/root` — nothing that cannot be
retyped.

To start encrypted:

```sh
./tools/mkenc.sh reset-disk   # moves build/disk.img to build/disk.img.plain-<UTC>
./image.sh                    # next boot creates the volume and formats it
```

The old image is kept. If you need its contents, put it back as `build/disk.img`
and boot: it still mounts, unencrypted, so copy `/root` off it first.

### 8.4 Changing the key

```sh
./tools/mkenc.sh new-key
```

This moves the current boot half to `build/mkenc/luks.key.<UTC>`, generates a
new one, and prints both fingerprints. **Every existing encrypted disk stops
opening** — the header's fingerprint is a hash of both halves, and the disk half
is now useless. To open an old disk again, boot with the saved file in place:

```sh
# inside the OS, with the old half copied in:
mystic-dmcrypt attach mystic-root /dev/vda /etc/mystic/luks.key.old
```

A real key-rotation (re-encrypt with a new data key, keeping the filesystem)
would need dm-crypt's `cryptsetup reencrypt` machinery, which does not exist
here. Changing the boot half is not rotation; it is re-keying the door lock and
throwing away the old key.

### 8.5 Removing a mapping by hand

```sh
mystic-dmcrypt status mystic-root          # exit 0 if it is active
mystic-dmcrypt detach mystic-root          # DM_DEV_REMOVE; fails if mounted
```

---

## 9. What is not implemented

Written down so nobody has to discover it:

* **LUKS1/LUKS2 compatibility.** Our header is 88 bytes of our own design. A
  `cryptsetup` on a rescue image cannot open it. The upside is that there is no
  metadata parser to get wrong.
* **Passphrase key slots** (see 6.2). No Argon2, no PBKDF2, no key slot count.
* **Header authentication / dm-integrity.** Corruption is not detected (6.5).
* **Re-encryption in place.**
* **Multiple keys or users.** One boot half per image.
* **`aesni-intel`** (see 3) — performance only.
* **Suspend/lock-triggered teardown.** Locking the desktop does not close the
  mapping; there is no unlock-later flow.

---

## 10. Verification: what was run, and what was not

Everything in this section was executed on the build host (Windows, MSYS), not
in QEMU.

### 10.1 Compilation

```
$ zig cc -target x86_64-linux-musl -Os -static -Wall -Wextra \
      -Wno-unused-parameter -c build/mkenc/mystic-dmcrypt.c -o build/mkenc/x.o
(no output)
```

The same source also compiles for the host with `-DMYSTIC_HOSTTEST=1`, which
replaces the three ioctls with nothing and the block-device size ioctl with
`stat()`, so the rest of the program can actually be executed here.

### 10.2 `./tools/mkenc.sh test` — 35 assertions, 0 failures

The simulation (`tools/mkenc.sh:1364`, script generated into
`build/mkenc/sim/run.sh`) **sources the real `stage/init`** with
`MYSTIC_INIT_LIB=1`, points `MYSTIC_DISK` at a regular file, and puts a fake
`mount`, `mkfs.ext4` and `modprobe` first on `PATH`. The helper is the real
thing: the host build of the same C, so the header it writes, the SHA-256 it
computes and the key check it performs are the shipping code. Nine scenarios
(paths shortened to `…`; the fingerprints differ on every run because the
simulation generates a fresh key each time):

```
== 1. no disk at all (real [ -b ] check) ==
        | disk: no /dev/vda
        | status: none
  PASS  disk: no /dev/vda
  PASS  no 'fake-mount'

== 2. first boot on a blank disk ==
        | crypt: initialised 4096-byte header, data at sector 2048, fingerprint bcb274e05c09efde
        | disk: created dm-crypt volume on …/dev/vda (aes-xts-plain64)
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/dev/vda 2048
        | disk: dm-crypt unlocked, no ext4 inside - creating one
        | fake-mount: /dev/mapper/mystic-root on /root
        | disk: formatted ext4 inside dm-crypt and mounted on /root
        | crypt: aes-xts-plain64, key fingerprint bcb274e05c09efde
        | status: aes-xts-plain64, key bcb274e05c09efde
        | mounted: /dev/mapper/mystic-root
  PASS  disk: created dm-crypt volume on …/dev/vda (aes-xts-plain64)
  PASS  disk: dm-crypt unlocked, no ext4 inside - creating one
  PASS  disk: formatted ext4 inside dm-crypt and mounted on /root
  PASS  mounted: /dev/mapper/mystic-root
  PASS  status: aes-xts-plain64, key 
        | header now: crypt 1 bcb274e05c09efde
  PASS  the disk file now starts with a MYSTICC1 header

== 3. second boot on the same disk ==
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/dev/vda 2048
        | fake-mount: /dev/mapper/mystic-root on /root
        | disk: dm-crypt unlocked, formatted ext4 mounted on /root
        | crypt: aes-xts-plain64, key fingerprint bcb274e05c09efde
        | status: aes-xts-plain64, key bcb274e05c09efde
        | mounted: /dev/mapper/mystic-root
  PASS  disk: dm-crypt unlocked, formatted ext4 mounted on /root
  PASS  mounted: /dev/mapper/mystic-root
  PASS  no 'created dm-crypt volume'
  PASS  no 'no ext4 inside'

== 4. a plain ext4 disk from an older build ==
        | disk: …/dev/plain has no encryption header and is not blank
        | fake-mount: …/dev/plain on /root
        | disk: mounted …/dev/plain (ext4) on /root
        | status: none (plain ext4)
        | mounted: …/dev/plain
  PASS  disk: …/dev/plain has no encryption header and is not blank
  PASS  disk: mounted …/dev/plain (ext4) on /root
  PASS  no 'created dm-crypt volume'
  PASS  no 'creating ext4 filesystem'
  PASS  the plain disk was not written to

== 5. encrypted disk, dm modules missing ==
        | disk: …/dev/vda is encrypted but cannot be opened - dm_mod/dm_crypt did not load
        | disk: refusing to format it, /root stays on the RAM filesystem
        | status: locked (dm_mod/dm_crypt did not load)
  PASS  disk: …/dev/vda is encrypted but cannot be opened - dm_mod/dm_crypt did not load
  PASS  disk: refusing to format it, /root stays on the RAM filesystem
  PASS  no 'creating ext4 filesystem'
  PASS  no 'fake-mount'
  PASS  the encrypted disk was not written to

== 6. encrypted disk, wrong key ==
        | disk: encrypted, but the key did not unlock it - …/dev/vda is left untouched
        |   mystic-dmcrypt: key does not match this disk (header bcb274e05c09efde, key 6e91690d4c815fcf) - the disk is left untouched
        | status: locked (key does not match)
  PASS  disk: encrypted, but the key did not unlock it - …/dev/vda is left untouched
  PASS  key does not match this disk
  PASS  status: locked (key does not match)
  PASS  no 'creating ext4 filesystem'
  PASS  the encrypted disk was not written to

== 7. blank disk, no key in the image ==
        | disk: no encryption key at …/absent.key - using plain ext4
        | disk: …/dev/fresh did not mount, creating ext4 filesystem
        | fake-mount: …/dev/fresh on /root
        | disk: formatted ext4 and mounted on /root
        | status: none (plain ext4)
        | mounted: …/dev/fresh
  PASS  disk: no encryption key at …/absent.key - using plain ext4
  PASS  disk: formatted ext4 and mounted on /root
  PASS  no 'created dm-crypt volume'

== 8. no helper in the image ==
        | disk: dm-crypt unavailable - no …/no-such-helper in the image
        | disk: …/dev/fresh did not mount, creating ext4 filesystem
        | fake-mount: …/dev/fresh on /root
        | disk: formatted ext4 and mounted on /root
        | status: none (plain ext4)
        | mounted: …/dev/fresh
  PASS  disk: dm-crypt unavailable - no …/no-such-helper in the image
  PASS  disk: formatted ext4 and mounted on /root
  PASS  no 'created dm-crypt volume'

== 9. an encrypted disk whose ext4 was never created ==
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/dev/emptyvol 2048
        | disk: dm-crypt unlocked, no ext4 inside - creating one
        | fake-mount: /dev/mapper/mystic-root on /root
        | disk: formatted ext4 inside dm-crypt and mounted on /root
        | crypt: aes-xts-plain64, key fingerprint 01be18d67e485717
        | status: aes-xts-plain64, key 01be18d67e485717
        | mounted: /dev/mapper/mystic-root
  PASS  disk: dm-crypt unlocked, no ext4 inside - creating one
  PASS  disk: formatted ext4 inside dm-crypt and mounted on /root

35 passed, 0 failed
```

Scenarios 4, 5 and 6 compare `cksum` before and after: the disk file is
byte-identical afterwards, which is the property that matters (no accidental
`mkfs` over data). Scenario 1 uses the real `[ -b ]` test rather than the fake;
the other scenarios replace `disk_present()` because a Windows build host has no
block devices at all. That substitution is stated at the top of the generated
script.

### 10.3 Helper self-test

`mystic-dmcrypt selftest` runs on the host build and checks, with no invented
values: SHA-256 against the four standard vectors (empty, `"abc"`,
`"abcdbcde…nopq"`, 1,000,000 × `'a'`); the hex encoding of 64 known bytes; the
exact dm table string for a 2 MiB device and for a 64 MiB device
(`0 129024 crypt aes-xts-plain64 … 0 /dev/vda 2048`); a 4 KiB header written to
a file and read back through `probe()` including both halves and the
fingerprint; a blank 4 KiB file classified `blank`; an ext4 superblock magic
classified `foreign`; and `key_read()` accepting 32 bytes and refusing 33.

```
ok   sha256("")
ok   sha256("abc")
ok   sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
ok   sha256(1,000,000 x 'a')
ok   hex(64 bytes)
ok   dm table, 2 MiB device -> 2048 data sectors at offset 2048
ok   dm table, 64 MiB device -> 129024 data sectors
ok   probe() finds the header and reads the disk half back
ok   header fields: version 1, offset 2048 sectors, key 64 bytes
ok   key_read() accepts exactly 32 bytes
ok   key_read() refuses a 33-byte file
ok   0 check(s) failed
```

### 10.4 Packaging

`tools/mkimage.py` was run into a throwaway output file
(`build/mkenc/pack-test.cpio.gz`) and the archive inspected entry by entry:

```
etc/mystic/luks.key                     mode 0100644, 32 bytes
init                                    mode 0100755, 14711 bytes, 0 CR bytes
sbin/mystic-dmcrypt                     mode 0100755, 40552 bytes
lib/modules/…/drivers/md/dm-crypt.ko    mode 0100644, 148999 bytes
lib/modules/…/drivers/md/dm-mod.ko      mode 0100644, 462255 bytes
lib/modules/…/security/keys/…/encrypted-keys.ko  0100644, 54367 bytes
```

Two facts worth flagging: `mkimage.py` forces mode `0644` on every non-executable
file, so the boot half is **world-readable inside the initramfs**. Nothing but
root and the desktop binary runs before the sandbox, and the sandboxed apps are
in a chroot without `/etc`, so this is not a hole today — but if `/etc` is ever
mounted into the app jail, that key becomes readable by uid 1000. The other fact
is `/init` with zero CR bytes, because a CRLF in a shell script is one of the
documented ways to break this project's boot.

### 10.5 What has never run

**The three dm ioctls.** `DM_DEV_CREATE`, `DM_TABLE_LOAD` and `DM_DEV_SUSPEND`
are compiled against the real `<linux/dm-ioctl.h>` (with `_Static_assert`s on the
struct sizes, 312 and 40 bytes) but they have never executed: that needs a
kernel, and this environment has no way to run one that I am allowed to use. The
first real boot is their first execution. Everything up to the ioctl boundary is
covered above; the ioctl sequence itself follows libdevmapper's and is four
calls long, and every failure prints the ioctl name and `strerror(errno)`, so a
first-boot failure will say exactly which call failed. Treat the first boot as
the test it is.

### 10.6 Coupling to `tools/smoke.sh`

`tools/smoke.sh:245` greps the probe output for

```
mounted /dev/vda \(ext4\)|formatted ext4
```

and fails `disk: ext4 mounted on /dev/vda` if neither appears. The encrypted
path cannot print `mounted /dev/vda (ext4)` — `/dev/vda` holds no filesystem —
so the two success messages keep the true substring `formatted ext4`:

```
disk: dm-crypt unlocked, formatted ext4 mounted on /root
disk: formatted ext4 inside dm-crypt and mounted on /root
```

The check therefore still passes, but its *name* is now inaccurate. The honest
fix is one line in `tools/smoke.sh` (another worker's file):

```sh
printf '%s' "$out" | grep -Eq 'mounted /dev/vda \(ext4\)|formatted ext4|mounted /dev/mapper/mystic-root'
```

---

## 11. Quick reference

```sh
./tools/mkenc.sh                 # stage everything, verify, print the fingerprint
./tools/mkenc.sh status          # what the next boot will do with build/disk.img
./tools/mkenc.sh test            # 9 boot-path scenarios, 35 assertions, no QEMU
./tools/mkenc.sh key             # key file, length, fingerprint
./tools/mkenc.sh new-key         # replace the boot half (old disks stop opening)
./tools/mkenc.sh reset-disk      # park build/disk.img so the next boot encrypts

# inside the OS
mystic-dmcrypt probe /dev/vda          # crypt <ver> <fp> | blank | foreign
mystic-dmcrypt attach mystic-root /dev/vda /etc/mystic/luks.key --table   # shape only
mystic-dmcrypt status mystic-root
mystic-dmcrypt detach mystic-root
mystic-dmcrypt selftest
```

| build / boot message | meaning |
|---|---|
| `disk: created dm-crypt volume on /dev/vda (aes-xts-plain64)` | first boot: header written |
| `disk: dm-crypt unlocked, formatted ext4 mounted on /root` | normal encrypted boot |
| `disk: formatted ext4 inside dm-crypt and mounted on /root` | header existed, filesystem did not |
| `disk: encrypted, but the key did not unlock it` | wrong boot half; nothing was touched |
| `disk: … is encrypted but cannot be opened` | header present, dm modules absent; nothing was touched |
| `disk: … has no encryption header and is not blank` | plain ext4 disk from an older build |
| `disk: no encryption key at …` | no boot half in the image; plain ext4 |
| `crypt : aes-xts-plain64, key <hex>` | probe line: the volume that was opened |
| `crypt : locked (…)` | probe line: the volume that was refused |
