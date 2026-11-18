# Encryption in Mystic OS

An encrypted `/root`, built from parts that are already in this tree, and an
honest account of what it does and does not protect.

**Short version.** `/dev/vda` becomes a dm-crypt volume
(`aes-xts-plain64`); the ext4 filesystem the rest of the OS writes to lives
*inside* that volume. The 64-byte data key is split in two: 32 bytes travel in
the initramfs (`/etc/mystic/luks.key`), 32 bytes are generated on the disk
itself at first boot and stored in an unencrypted 4 KiB header. Neither half
alone decrypts the disk. A copy of `build/disk.img` on its own is ciphertext.

The boot half can be **sealed with the lock-screen password**
(`./tools/mkenc.sh seal`): the initramfs then carries
`/etc/mystic/luks.key.sealed` instead of the key, and the boot asks for the
passphrase on `/dev/console` before mounting `/root`. That turns
"a copy of the initramfs is plaintext" into "a copy of the initramfs needs the
password", which is a real change; it does not make a weak password strong, and
section [6.3](#63-what-the-seal-is-worth) says exactly why.

Everything else — the running system, a copy of the disk *and* an unsealed
initramfs, anything that can read both halves — sees plaintext. Section
[6](#6-what-is-and-is-not-protected) is the part to read before believing
anything else.

Two invariants are enforced in `stage/init` and covered by the simulation
(section [10](#10-verification-what-was-run-and-what-was-not)):

* A device that carries the `MYSTICC1` header is **never** formatted, whatever
  is missing — the helper, the modules, the key, or a parsable header.
* Only a device that **provably holds no filesystem** is ever formatted. A
  failed mount is not that proof: the ext2/3/4 magic at byte 1080, a header
  marker written after the last successful `mkfs`, and an all-zero first 4 KiB
  are all checked first.

---

## 1. Run it

```sh
./tools/fetch-kernel.sh     # once: kernel + modules into build/cache and stage/
./tools/mkenc.sh            # stage dm-crypt modules, key, helper; verify
./tools/mkenc.sh test       # boot-path simulation, no QEMU
./tools/mkenc.sh status     # what the next boot will do with build/disk.img
./tools/mkenc.sh seal       # optional: require the lock-screen password at boot
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
| on-disk format + dm ioctls | `tools/mkenc.sh:101-1938` (the C source embedded in the tool) | `mystic-dmcrypt`: ~1,300 lines of C, no dependencies |
| compiled helper | `stage/sbin/mystic-dmcrypt` | static musl ELF, 55,720 bytes as built here |
| boot half of the key | `stage/etc/mystic/luks.key` | 32 bytes from `/dev/urandom`, created once by `mkenc.sh` |
| sealed boot half | `stage/etc/mystic/luks.key.sealed` | 112 bytes: the same key under PBKDF2-HMAC-SHA256 → ChaCha20 + HMAC-SHA256 |
| module staging | `tools/mkenc.sh:79` `stage_modules()` | copies `dm-mod.ko`, `encrypted-keys.ko`, `dm-crypt.ko` out of the cached kernel package |
| build / verify | `tools/mkenc.sh:1941` `build_helper()`, `:2076` `verify()` | exact compile command, ELF check, vermagic check, key length check, seal size check |
| boot logic | `stage/init:106-717` | the state machine in [7](#7-the-boot-sequence) |
| seal / unseal | `tools/mkenc.sh:2267` `cmd_seal()`, `:2302` `cmd_unseal()` | prompts, checks against `stage/etc/mystic/passwd`, never keeps the passphrase |
| boot-path simulation | `tools/mkenc.sh:2353` `cmd_test()` | runs `stage/init` against a fake `/dev`, fake `mount`/`mkfs`, real helper |

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
`stage/lib/modules/<ver>/kernel/drivers/md/` was empty before this work:
`tools/fetch-kernel.sh` copies a hand-written list of subtrees
(`MODULE_TREES`, `tools/fetch-kernel.sh:27-41`) and `kernel/drivers/md`,
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
| `mystic-dmcrypt` | nothing. musl, three dm ioctls plus SHA-256/HMAC/PBKDF2/ChaCha20 written out, 55,720 bytes |

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

`129024 = 131072 - 2048`: the first 1 MiB of the disk is the header area, and
sector 2048 of the mapping is sector 2048 of the block device. The kernel
reports the mapping's major:minor in the ioctl reply, so the helper creates
`/dev/mapper/mystic-root` itself with `mknod` if devtmpfs did not.

The LUKS header format is *not* implemented. This is not a LUKS volume;
`cryptsetup luksDump` will not read it. What we give up compared with LUKS:
multiple key slots, passphrase-based key derivation (Argon2), an authenticated
header, and interoperability. See [9](#9-what-is-not-implemented).

### 4.1 Exit codes (the shell branches on these, so they are part of the ABI)

| `mystic-dmcrypt probe <dev>` | |
|---|---|
| 0 | `crypt <version> <fingerprint>` — a MYSTICC1 header that parses |
| 10 | `blank` — the first 4 KiB is all zero |
| 11 | `fs` — ext2/3/4 superblock magic at byte 0x438 |
| 12 | `unknown` — not blank, no filesystem magic, no header |
| 1 | the device could not be opened, or its header is unparseable |

| `mystic-dmcrypt attach <name> <dev> <key>` | |
|---|---|
| 0 | the mapping is live (or was already live) |
| 2 | the key file is missing, unreadable, or not exactly 32 bytes |
| 3 | the key does not match the header fingerprint — nothing was touched |
| 4 | the device mapper refused: no `/dev/mapper/control`, an ioctl failed, or the node could not be created |
| 1 | bad arguments, no header, or the device is too small |

Codes 2, 3 and 4 are what let `stage/init` say *why* a volume did not open
instead of blaming the key for everything. See [7](#7-the-boot-sequence).

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
| `0x58` | 8 | ASCII `MYSTICFS` once a filesystem has been created inside the mapping |
| `0x60` | 4 | marker version (`1`) |
| `0x64` | 3996 | zero |

The `MYSTICFS` marker is written by `mystic-dmcrypt markfs` immediately after
`mkfs.ext4` + mount succeed inside the mapping. It is what lets the next boot
tell "this volume was formatted once and its filesystem is gone" from "this
volume was never formatted", even when the filesystem inside was not ext4. A
volume without the marker and without the ext4 magic is formatted; anything
else is only ever mounted. An unmarked header from before this field existed is
all zeros there, which reads as "not marked" — harmlessly, because such a
volume also has its ext4 magic.

The data key is the two halves concatenated, 64 bytes, used directly as the
XTS key (AES-256-XTS: two 256-bit keys). No KDF is applied, because both halves
are already 256 bits from `/dev/urandom`; there is nothing to stretch.

The fingerprint at `0x38` exists for one reason: so a **wrong key is detected
before anything is formatted**. `stage/init` never formats a disk whose header
it could not open. It is a verifier, so as long as both halves are 256-bit
random it hides nothing useful. It *would* be an offline-crackable oracle if
the key were a passphrase — which is why `key_read()`
(`tools/mkenc.sh:476`) refuses any key file that is not exactly 32 bytes rather
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

The boot half has three possible shapes, and which one is in the image decides
what a boot does:

| boot half | in the initramfs | how the boot gets it |
|---|---|---|
| plaintext | `/etc/mystic/luks.key` | read it. No prompt, unattended boots. |
| sealed | `/etc/mystic/luks.key.sealed` | ask for the lock-screen password, unseal into RAM. Unattended boots **cannot** open the volume. |
| absent | nothing | the volume cannot be opened; a blank disk is not formatted |

The sealing form is described in [6.2](#62-the-passphrase-prompt-at-boot) and
what it is worth in [6.3](#63-what-the-seal-is-worth).

* A copy of `build/disk.img` alone: **protected**. The ciphertext is useless and
  the header only gives away 32 random bytes and a SHA-256 of the whole key.
* A copy of the initramfs alone (`build/initramfs.cpio.gz`): **protected when
  the boot half is sealed** (the seal needs the passphrase), **plaintext when it
  is not** (the 32 bytes are right there).
* Both files, unsealed key: **not protected at all**. They are enough to rebuild
  the key and read everything.
* Both files, sealed key: protected by the passphrase alone. An attacker can
  verify guesses offline against either the seal or
  `/etc/mystic/passwd` (whichever is cheaper), so the passphrase is the only
  secret left. See [6.3](#63-what-the-seal-is-worth).

If you copy the key file onto the disk (or into the same archive as the disk
image), you have implemented the "key stored on the disk" variant and the
protection against offline inspection of that disk is *zero*.

**The key file is already tracked by git in this repository** — not merely
unignored. `git ls-files stage/etc/mystic/luks.key` lists it, so adding it to
`.gitignore` will not help. Until it is untracked, every commit publishes the
boot half:

```sh
git rm --cached stage/etc/mystic/luks.key   # keep the working copy
./tools/mkenc.sh new-key                    # and retire the published half
```

A published boot half is not enough to read a disk image — that is the point of
the split — but it is half of the secret handed out for free. **Sealing does not
fix this**: the sealed file is also readable from the repository, and a published
seal plus a published `passwd` line is an offline guessing target. If the seal is
meant to protect anything, neither file may be public.

### 6.2 The passphrase prompt at boot

The boot asks for a passphrase **only when the boot half is sealed**, i.e. when
`stage/etc/mystic/luks.key.sealed` is in the image and `stage/etc/mystic/luks.key`
is not. `./tools/mkenc.sh seal` creates that state; `./tools/mkenc.sh unseal`
undoes it. The unsealed form stays the default because the unattended paths
(`./image.sh probe`, `tools/smoke.sh`) depend on it.

The prompt is `stage/init:227` `crypt_key_ready()`, which runs before the
mapping is created:

```
key: unsealed /etc/mystic/luks.key.sealed into /run/mystic/luks.key (RAM only)
```

* It reads from `/dev/console` (the kernel `console=` device — `ttyS0` with the
  command lines in `image.sh`), opens it, and turns echo off with `termios`
  inside the helper; the passphrase never appears in argv, in an environment
  variable, or in a shell variable in the shipping path. The helper wipes its
  buffer afterwards.
* The typed passphrase is checked against `/etc/mystic/passwd` using the **same**
  derivation the lock screen runs (`src/sha256.c` `mystic_hash_password`,
  `salt:rounds:hash`, 20000 rounds). The simulation cross-checks that: it makes
  a `passwd` line with the shipping `src/sha256.c` and requires the helper's own
  check to accept it (`tools/mkenc.sh test`, scenario 19-24).
* It then unseals the 32-byte boot half into `/run/mystic/luks.key` (mode 0600,
  a ramfs path — never on the disk), hands it to `mystic-dmcrypt attach`, and
  overwrites the file with zeros and unlinks it. dm-crypt keeps the key in
  kernel memory, which this cannot and does not clear.
* Three attempts, then the boot gives up, says so, and leaves `/root` on the
  RAM filesystem. Nothing is formatted, nothing is written.

When it does **not** prompt, and why:

* `mystic.probe=1` on the kernel command line (`./image.sh probe`). That mode is
  automated and headless; a blocking read would hang it until its `timeout`
  killed QEMU. It prints
  `disk: /etc/mystic/luks.key.sealed needs the passphrase; probe mode does not prompt`
  and reports `crypt : locked (no usable key)`. A sealed image therefore cannot
  be probe-verified without a human; that is the price of the seal.
* No `/dev/console` in the image (a rescue build). It says which path was
  missing.
* The plaintext key is present; then the prompt is skipped entirely, because
  that is the unattended path.

**Where the prompt appears.** `/dev/console` is whatever `console=` says.
`image.sh run` boots with `console=ttyS0` and no `-serial`, so QEMU's default
serial backend decides: normally a separate virtual-console window, not the
desktop window. Two consequences worth knowing before sealing:

* if you cannot see that window, you cannot type the passphrase; the boot then
  finishes with `/root` unmounted and says so (no data loss, no progress
  either);
* to get the prompt in the main window, boot with `console=tty0` instead of
  `console=ttyS0` (the VGA text console is still bound at that point — the
  desktop unbinds it later). The exact QEMU command is in `image.sh`; that file
  belongs to another worker, so the flag is documented here and not changed
  there.

### 6.3 What the seal is worth

The sealed file (`/etc/mystic/luks.key.sealed`, 112 bytes) is:

```
0x00  8  "MYSTICKS"
0x08  4  version (1)
0x0c  4  PBKDF2-HMAC-SHA256 rounds (50000 by default, recorded in the file)
0x10 16  salt
0x20 12  ChaCha20 nonce (RFC 8439, 96-bit)
0x2c  4  flags (0)
0x30 32  ChaCha20 ciphertext of the 32-byte boot half
0x50 32  HMAC-SHA256(mac_key, bytes 0x00..0x4f)
```

`PBKDF2-HMAC-SHA256(passphrase, salt, rounds)` produces 64 bytes: the first 32
encrypt, the last 32 authenticate (encrypt-then-MAC). The primitives are
from published standards and are checked against their published vectors in
`mystic-dmcrypt selftest` (section [10.3](#103-helper-self-test)): SHA-256
(FIPS 180-4), HMAC-SHA256 (RFC 4231 case 2), PBKDF2-HMAC-SHA256 (the published
`password`/`salt` vectors for 1, 2 and 4096 rounds) and ChaCha20 (RFC 8439
§2.4.2, cross-checked against OpenSSL for the same key/nonce/counter). Tampering
with one ciphertext byte or with the recorded round count is detected by the
HMAC — the selftest checks both.

What it buys:

* a leaked initramfs is no longer the boot half in the clear;
* the boot cannot mount `/root` without a human typing the password;
* the seal is bound to the passphrase, not to `/etc/mystic/passwd`: knowing the
  stored hash does **not** let you derive the key (the KDF input is the typed
  passphrase, not the hash). This is the one place where the design is
  deliberately stronger than "use the hash file as the key".

What it does **not** buy, stated plainly:

* **A weak password stays weak.** The cheapest verifier in a leaked initramfs is
  `/etc/mystic/passwd`: SHA-256 iterated 20000 times, with no memory hardness —
  exactly the shape of hash that a GPU is fastest at, so an offline attacker
  guesses at that cost, not at the seal's. The seal's own KDF is 50000 rounds of
  PBKDF2-HMAC-SHA256 (~0.15 s on the build host, measured), chosen so that the
  seal is never cheaper to attack than the passwd file — a bigger number here
  would slow the honest boot and not the attacker. If the password is `1234`,
  the disk is open regardless.
* **It does not defend against someone who watches you type**, or against root
  once the volume is mounted.
* **Changing the lock-screen password does not re-seal.** The seal still opens
  with the passphrase it was made with (and the helper warns that it no longer
  matches `/etc/mystic/passwd`); run `./tools/mkenc.sh seal` again afterwards.
  That is the same behaviour as LUKS, where the login password and the disk
  passphrase are separate secrets.
* **Losing the passphrase loses the disk.** There is no recovery key and no key
  slot. `luks.key.sealed` is 112 bytes; back it up together with the passphrase
  — the backup alone is useless, which is the point.

### 6.4 The lock screen is not encryption

`src/main.c`'s lock screen (`try_unlock`, `/etc/mystic/passwd`, SHA-256 with
20000 rounds) is a **UI lock**. It stops someone using the desktop; it does not
touch the disk, the key, or the dm-crypt mapping. When the desktop is locked the
filesystem is still mounted and decrypted. Do not confuse the two, and do not
believe the PIN protects `/root` at rest.

### 6.5 Threat model

| attacker has | what they get |
|---|---|
| `build/disk.img` (the realistic offline case) | ciphertext. `/root`, `/root/notes`, `boots.txt`: unreadable. |
| the initramfs, unsealed boot half | plaintext. The 32 bytes are in the archive. |
| the initramfs, sealed boot half | a 112-byte blob and a 20000-round password verifier; needs the passphrase (see 6.3). |
| `build/disk.img` + an **unsealed** initramfs | plaintext. Both halves. |
| `build/disk.img` + a **sealed** initramfs | ciphertext until the passphrase is guessed. The disk half is useless alone. |
| a copy of this git repository (the boot half is tracked, see 6.1) | the boot half only — not enough without a disk image, but it is half the work, and it defeats the seal if the seal was published too. |
| root inside the running OS | everything. The mapping is open, and the kernel holds the key for the live mapping. dm-crypt is not a defence against a live root. |
| the physical machine while it is running | everything, including a cold-boot or DMA attack on RAM. |
| write access to `build/disk.img` | they cannot read it, but they can **corrupt** it: there is no authentication (see 6.6). |
| a way to make the boot key (or its passphrase) disappear | denial of service: the volume will not open, and the boot refuses to format it. Data is not destroyed, but `/root` is gone until the key is restored. |

### 6.6 What cryptography is *not* here

* **No authentication.** `aes-xts-plain64` is unauthenticated encryption. An
  attacker with write access to the disk can flip ciphertext bits; ext4 will
  read garbage where that block was, and might mount or might not. dm-crypt
  cannot tell you the data was modified. `dm-integrity` (or an AEAD mode) would
  be needed; it costs a second mapping and is not implemented.
* **No header authentication.** The 1 MiB header is plaintext and unauthenticated.
  The fingerprint catches a *wrong key*, not a *tampered header*. What tampering
  does now: flipping the version byte, the offset, the cipher id or the disk
  half makes the header unparseable or the fingerprint wrong, and the boot
  **refuses** to touch the disk (simulation scenarios 12 and 13). The one
  remaining case is a header that is zeroed completely: the disk then reads as
  `blank`, and a blank disk is formatted (scenario 2). Zeroing the header is
  equivalent to deleting the key file — it is a wipe, not a bypass. The
  `MYSTICFS` marker is unauthenticated too: an attacker who can write the header
  can set or clear it, which changes only whether `mkfs` is allowed, never
  whether data is revealed.
* **The seal authenticates itself, but not the data.** The HMAC covers the
  sealed blob; it says nothing about the 32 bytes it contains or the disk.
* **No anti-rollback.** An old copy of the disk image opens with the current key.
* **No protection of the swap or RAM.** There is no swap; the desktop keeps
  `/root` contents in page cache in the clear like any Linux.

---

## 7. The boot sequence

`stage/init:553` `setup_disk()` classifies the device, then branches. The
classification has two independent sources, and that is deliberate:

1. `mystic-dmcrypt probe` (`stage/init:314`) — the normal path.
2. `dev_probe_shell` (`stage/init:185`) — three reads of the device with
   `dd`/`tr`/`cksum` (no `od`: its `-j`/`-N`/`-v` extensions are not guaranteed
   by a busybox build) answering header / blank / other. It is used when the
   helper is **missing from the image** (a `git clean` removes the ignored
   `stage/sbin/`) or when the helper fails.

So a device whose first eight bytes are `MYSTICC1` is recognised as ours even
with no helper, no modules and no key. It is never handed to `mkfs`.

Key resolution (`crypt_key_ready`, `stage/init:227`) happens before the mapping
is created, so "no key" and "no disk" are never confused:

| key state | what init does | message |
|---|---|---|
| plaintext `/etc/mystic/luks.key`, 32 bytes | use it | `key: /etc/mystic/luks.key (32 bytes, not sealed)` |
| sealed, console present, passphrase right | unseal to `/run/mystic/luks.key`, use it, wipe it after `attach` | `key: unsealed /etc/mystic/luks.key.sealed into /run/mystic/luks.key (RAM only)` |
| sealed, wrong passphrase | three attempts, then refuse | `disk: that is not the lock-screen password (attempt N of 3)` … `disk: giving up after 3 attempts - the volume stays locked` |
| sealed, no console | refuse | `disk: … needs the passphrase, and there is no console at /dev/console` |
| sealed, probe mode | refuse | `disk: … needs the passphrase; probe mode does not prompt` |
| sealed, helper missing | refuse | `disk: … needs /sbin/mystic-dmcrypt to be unsealed, and it is not in the image` |
| no key at all | refuse on an encrypted disk; on a **blank** disk the pre-encryption path, stated out loud | `disk: no key at … and no sealed key at …` or `disk: this image has no boot key, so /dev/vda stays UNENCRYPTED` |

| disk state | meaning | what init does | message |
|---|---|---|---|
| no device | `[ -b /dev/vda ]` false | nothing | `disk: no /dev/vda` *(unchanged)* |
| `blank` | first 4 KiB all zero | write header, open mapping, `mkfs.ext4`, mount, mark | `disk: created dm-crypt volume on /dev/vda (aes-xts-plain64)` then `disk: formatted ext4 inside dm-crypt and mounted on /root` |
| `crypt <fp>` | our header, fingerprint matches | open mapping, mount | `disk: dm-crypt unlocked, formatted ext4 mounted on /root` |
| `crypt <fp>`, no ext4 magic inside, no `MYSTICFS` marker | header exists, nothing was ever formatted in it | open mapping, `mkfs.ext4`, mount, mark | `disk: dm-crypt unlocked, no filesystem inside - creating one` |
| `crypt <fp>`, ext4 magic present but mount failed | a filesystem that will not mount | **refuse**: no mkfs, no mount | `disk: dm-crypt unlocked, ext4 is there but did not mount - refusing to format it` |
| `crypt <fp>`, no ext4 magic but the `MYSTICFS` marker is set | a filesystem was created here once and is gone | **refuse** | `disk: dm-crypt unlocked, no ext4 magic, but its header records that a` / `disk: filesystem was created in it once - refusing to format it` |
| `locked: no helper` | header present, helper missing | **refuse** | `disk: … is encrypted but cannot be opened - no … in the image, and … carries a MYSTICC1 header` |
| `locked: no modules` | header present, `dm_mod`/`dm_crypt` did not load | **refuse** | `disk: … is encrypted but cannot be opened - dm_mod/dm_crypt did not load` |
| `locked: probe failed` | header present, helper cannot parse it | **refuse** | `disk: … is encrypted but cannot be opened - the probe failed, and … carries a MYSTICC1 header` |
| `locked (key does not match)` | header opened, key differs (exit 3) | **refuse** | `disk: encrypted, but the key did not unlock it` |
| `locked (no key)` | header present, no plaintext and no sealed key in the image | **refuse** | `disk: no key at … and no sealed key at …` |
| `locked (passphrase refused)` | sealed key, three failed attempts or a damaged seal | **refuse** | `disk: giving up after 3 attempts - the volume stays locked` |
| `locked (dm-crypt error)` | ioctl/control-node failure (exit 4) | **refuse** | `disk: dm-crypt could not create the mapping` |
| `fs` | helper said "ext4 magic, no header" | mount; if that fails, **refuse** (see rule 3) | `disk: … has no encryption header, so it is not encrypted`, then `disk: mounted … (ext4) on /root` |
| `keep: unknown` | helper said "not blank, no filesystem magic, no header" | try to mount; **refuse to format** | `disk: … is not blank, not ext4 and carries no MYSTICC1 header` then `disk: … did not mount and is not blank - refusing to format it` |
| `keep: other` | no helper, and the device is neither blank nor ours | try to mount; **refuse to format** | `disk: no … in the image, and … is not blank and carries no MYSTICC1 header` |
| `unavailable` | blank disk, no helper or no modules | the original path: mkfs plain, said out loud | `disk: dm-crypt unavailable - …` / `disk: … is blank, so this creates an UNENCRYPTED /root` then `disk: formatted ext4 and mounted on /root` |
| `blank`, key exists but cannot be created | `init` failed | **refuse**; if the header got written first, say it will not be formatted | `disk: could not create the encrypted volume - … is left untouched` |

Four rules make this safe rather than clever:

1. **A device that carries our header is never formatted**, whatever is missing:
   not the helper, not the modules, not the key, not a parsable header. This is
   `crypt_state()`, which asks the shell probes first and never trusts an exit
   code to decide that a volume is not at stake, and `crypt_create()`, which
   re-reads the header after a failed `init` because `init` writes the header
   before anything else.
2. **A failed mount is not proof that there is no filesystem.** Inside the
   mapping the boot checks the ext2/3/4 magic at byte 1080 *and* the header's
   `MYSTICFS` marker, and on a plain device it checks for the header, the ext4
   magic and an all-zero first 4 KiB. Only when all of that says "nothing here"
   does `mkfs.ext4 -F` run — with `-F` given explicitly, because the checks
   above already decided, not because mkfs was told to ignore what it found.
   The old KNOWN RISK ("formats whenever the mount fails on a disk that is blank
   or carries an ext4 superblock") is gone: that was the P0 data-loss path the
   audit proved, and scenario 17 covers it.
3. **The pre-encryption path is kept for a *mountable* ext4 disk** (`fs`): it
   mounts, and the boot is unencrypted, unchanged. An ext4 disk that does **not**
   mount is now refused instead of reformatted (`disk: … has an ext4 superblock
   but did not mount - refusing to format it`) — that is the second half of
   P0-2, and the advice is `e2fsck -y`, not `mkfs`.
4. **A blank disk with no key in the image does not silently become encrypted**
   with a key that will be lost at reboot. There is no persistent place to put a
   key generated at runtime (the initramfs is a read-only cpio), so init says
   `disk: this image has no boot key, so /dev/vda stays UNENCRYPTED` and takes
   the old path. If a key exists but cannot be used (sealed and no console,
   helper missing, passphrase refused), the boot refuses rather than downgrading
   silently.

The probe state is also printed, so a boot log says which one happened:

```
crypt    : aes-xts-plain64, key 6c67030f117e8ce0     (example, from a simulation run)
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
disk: /dev/vda has no encryption header, so it is not encrypted
disk: mounted /dev/vda (ext4) on /root
```

and the probe line reports `crypt    : none (plain ext4)`.

If that disk has an ext4 superblock but does **not** mount, the boot no longer
reformats it (that was the second P0):

```
disk: /dev/vda has no encryption header, so it is not encrypted
disk: /dev/vda has an ext4 superblock but did not mount - refusing to format it
disk: run e2fsck -y on it, or ./tools/mkenc.sh reset-disk for a fresh disk
```

There is no in-place migration path in this tree, and it is worth being blunt
about why: the whole device is already occupied by the plaintext filesystem, so
there is nowhere to put the ciphertext, and the image has no `losetup`, no
`rsync` and no second device to shuffle data through. The demo disk holds
`/root/mystic-ui.conf`, `/root/mystic-wallpaper`, `/root/mystic-current-theme`,
`boots.txt` and whatever the apps made in `/root` — nothing that cannot be
retyped.

To start encrypted, **stage first** — `reset-disk` refuses to move the disk
unless the key, the helper and the modules are all in place, because a fresh disk
built without them would come back unencrypted and say nothing about it:

```sh
./tools/mkenc.sh              # key + helper + modules, or reset-disk will refuse
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

`./tools/mkenc.sh status` says so before you boot: it runs `attach --table`
against `build/disk.img`, which verifies the fingerprint and stops before any
ioctl, so a wrong boot half prints `next boot: will NOT open it - …` instead of
a cheerful "unlocks it".

A real key-rotation (re-encrypt with a new data key, keeping the filesystem)
would need dm-crypt's `cryptsetup reencrypt` machinery, which does not exist
here. Changing the boot half is not rotation; it is re-keying the door lock and
throwing away the old key.

### 8.5 Removing a mapping by hand

```sh
mystic-dmcrypt status mystic-root          # exit 0 if it is active
mystic-dmcrypt detach mystic-root          # DM_DEV_REMOVE; fails if mounted
```

### 8.6 Sealing the boot half with a passphrase

The plaintext half makes every boot unattended. `seal` replaces it with the
112-byte sealed blob, keyed to the **lock-screen password**:

```
$ ./tools/mkenc.sh seal
passphrase (the lock-screen password):
again:
sealed stage/etc/mystic/luks.key as stage/etc/mystic/luks.key.sealed: 50000 rounds, key fingerprint d07c46c21884baa9
==> sealed: stage/etc/mystic/luks.key.sealed (key fp d07c46c21884baa9)
==> the plaintext half is gone; the next boot asks for the passphrase before
==> mounting /root. Rebuild the image (./image.sh) so the initramfs gets it.
WARNING: an encrypted disk keeps opening: the seal holds the same 32 bytes.
WARNING: changing the lock-screen password does NOT re-seal: run seal again after it.
```

It refuses a passphrase that is not the lock-screen one, before touching
anything:

```
$ ./tools/mkenc.sh seal
passphrase (the lock-screen password):
mystic-dmcrypt: wrong password
error: that is not the lock-screen password - nothing was sealed
```

and refuses to seal when the lock screen has no password at all, because such a
seal could never be opened. `seal` asks twice and never echoes; the passphrase
never reaches argv, the environment or a file in the shipping path. (For a
scripted run only, `MYSTIC_SEAL_PASS_FILE=<file>` supplies it; the script prints
a warning when it does.)

`./tools/mkenc.sh status` then says what the next boot will do, reading the
round count out of the blob rather than assuming it:

```
boot key half  : stage/etc/mystic/luks.key.sealed (sealed, 50000 PBKDF2-HMAC-SHA256 rounds)
                 the boot asks for the lock-screen passphrase
                 in probe mode it cannot prompt, so /root stays unmounted
```

To go back to unattended boots, `./tools/mkenc.sh unseal` asks for the
passphrase, writes the 32 bytes back to `stage/etc/mystic/luks.key` and removes
the blob. The key is byte-identical (same fingerprint), so an existing encrypted
disk keeps opening — a verified round trip:

```
$ cksum stage/etc/mystic/luks.key
3840347231 32 stage/etc/mystic/luks.key
$ ./tools/mkenc.sh seal && ./tools/mkenc.sh unseal
...
$ cksum stage/etc/mystic/luks.key
3840347231 32 stage/etc/mystic/luks.key
```

Rebuild the image (`./image.sh`) after either command: the initramfs is what
carries the file.

**If you forget the passphrase, the volume is gone.** There is no recovery key
and no second key slot. A copy of `luks.key.sealed` does not help without the
passphrase, which is the point.

---

## 9. What is not implemented

Written down so nobody has to discover it:

* **LUKS1/LUKS2 compatibility.** Our header is our own design. A `cryptsetup` on
  a rescue image cannot open it. The upside is that there is no metadata parser
  to get wrong.
* **A key slot count / multiple keys / multiple users.** One boot half per
  image, sealed or not. `seal` re-seals with a new passphrase but does not
  create a second slot, and there is no recovery key.
* **Argon2 or scrypt.** The seal uses PBKDF2-HMAC-SHA256 (50000 rounds) and the
  lock screen uses SHA-256 iterated 20000 times. Both are CPU-only and
  GPU-friendly; section [6.3](#63-what-the-seal-is-worth) says what that means
  for a weak password.
* **Header authentication / dm-integrity.** Corruption is not detected (6.6).
* **Re-encryption in place.**
* **`aesni-intel`** (see 3) — performance only.
* **Suspend/lock-triggered teardown.** Locking the desktop does not close the
  mapping; there is no unlock-later flow.
* **A recovery path for a forgotten passphrase.** By design; see 8.6.

---

## 10. Verification: what was run, and what was not

Everything in this section was executed on the build host (Windows, MSYS), not
in QEMU.

### 10.1 Compilation

```
$ zig cc -target x86_64-linux-musl -Os -static -Isrc -Wall -Wextra \
      -Wno-unused-parameter -c build/mkenc/mystic-dmcrypt.c -o build/mkenc/x.o
(no output)
```

The same source also compiles for the host with `-DMYSTIC_HOSTTEST=1`, which
replaces the three ioctls with a printed table and the block-device size ioctl
with `stat()`, so the rest of the program runs on this machine.

`./tools/mkenc.sh elf <file>` prints the ELF verdict and exits non-zero when the
file needs a dynamic loader, which is how the static-helper check is tested:

```
$ ./tools/mkenc.sh elf stage/sbin/mystic-dmcrypt
ET_EXEC, 6 program header(s), no PT_INTERP (static)          exit 0
$ ./tools/mkenc.sh elf <a PT_INTERP ELF>
ET_EXEC, 1 program header(s), PT_INTERP present (DYNAMIC)    exit 1
```

With that ELF planted as `stage/sbin/mystic-dmcrypt` and the compile step
skipped, `./tools/mkenc.sh` prints
`WARNING: stage/sbin/mystic-dmcrypt is not a static executable (see above)` and
`error: verification failed` — the check can fail.

### 10.2 `./tools/mkenc.sh test` — 106 assertions, 0 failures

The simulation (`tools/mkenc.sh:2353`, script generated into
`build/mkenc/sim/run.sh`) **sources the real `stage/init`** with
`MYSTIC_INIT_LIB=1`, points `MYSTIC_DISK` at a regular file, and puts a fake
`mount`, `mkfs.ext4` and `modprobe` first on `PATH`. Four things make the
evidence real rather than cosmetic:

* the helper is the **host build of the shipping C**, so the header it writes,
  the SHA-256 it computes, the sealed-key codec and the key check it performs
  are the same code that boots;
* the fake `mkfs.ext4` writes the actual ext4 superblock magic into the device
  and the fake `mount` decides from that magic, so a format is visible to
  `cksum` and "there is a filesystem but it will not mount" is reachable;
* the `passwd` line used by the sealed-key scenarios is produced by
  **`src/sha256.c` itself** (compiled into `build/mkenc/mystic-pwline`), so the
  helper's password check has to agree with the code the desktop lock screen
  runs — it is not compared against a second copy of the algorithm;
* the passphrase is typed into a file standing in for `/dev/console`, which is
  the same code path (`--prompt`) the real boot uses, minus the termios echo
  flag.

Twenty-five scenarios, 106 assertions. The four boot paths the task names,
verbatim (paths shortened to `…`; every run generates fresh keys, so the
fingerprints differ from yours):

```
== 1. no disk at all (real [ -b ] check) ==
        | disk: no /dev/vda
        | status: none
  PASS  disk: no /dev/vda
  PASS  no 'fake-mount'

== 2. first boot on a blank disk ==
        | key: … (32 bytes, not sealed)
        | crypt: initialised 4096-byte header, data at sector 2048, fingerprint 3cf110be65ddef92
        | disk: created dm-crypt volume on …/vda (aes-xts-plain64)
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/vda 2048
        | disk: dm-crypt unlocked, no filesystem inside - creating one
        | fake-mount: … on /root
        | crypt: marked …/vda: a filesystem was created inside its mapping
        | disk: formatted ext4 inside dm-crypt and mounted on /root
        | crypt: aes-xts-plain64, key fingerprint 3cf110be65ddef92
        | status: aes-xts-plain64, key 3cf110be65ddef92
        | mounted: …
  PASS  disk: created dm-crypt volume on …/vda (aes-xts-plain64)
  PASS  disk: dm-crypt unlocked, no filesystem inside - creating one
  PASS  disk: formatted ext4 inside dm-crypt and mounted on /root
  PASS  mounted: …
  PASS  status: aes-xts-plain64, key 
  PASS  crypt: marked …/vda: a filesystem was created inside its mapping
        | header now: crypt 1 3cf110be65ddef92
  PASS  the disk file now starts with a MYSTICC1 header
  PASS  the ext4 superblock was written inside the mapping
  PASS  the raw disk has no ext4 superblock at byte 1080 (ciphertext starts there)

== 3. second boot on the same disk ==
        | key: … (32 bytes, not sealed)
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/vda 2048
        | fake-mount: … on /root
        | disk: dm-crypt unlocked, formatted ext4 mounted on /root
        | crypt: aes-xts-plain64, key fingerprint 3cf110be65ddef92
        | status: aes-xts-plain64, key 3cf110be65ddef92
        | mounted: …
  PASS  disk: dm-crypt unlocked, formatted ext4 mounted on /root
  PASS  mounted: …
  PASS  no 'created dm-crypt volume'
  PASS  no 'no filesystem inside'

== 6. encrypted disk, wrong key ==
        | key: … (32 bytes, not sealed)
        | disk: encrypted, but the key did not unlock it - …/vda is left untouched
        |   mystic-dmcrypt: key does not match this disk (header 3cf110be65ddef92, key 8e3b13213f3fd787) - the disk is left untouched
        | status: locked (key does not match)
  PASS  disk: encrypted, but the key did not unlock it - …/vda is left untouched
  PASS  key does not match this disk
  PASS  status: locked (key does not match)
  PASS  no 'creating ext4 filesystem'
  PASS  the encrypted disk was not written to

```

The two data-loss paths the audit proved, and the code that now refuses to
walk into them:

```
== 17. a plain ext4 disk whose mount fails = 
        | disk: …/plainsick has no encryption header, so it is not encrypted
        | disk: …/plainsick has an ext4 superblock but did not mount - refusing to format it
        | disk: run e2fsck -y on it, or ./tools/mkenc.sh reset-disk for a fresh disk
        | status: present but not mountable (left untouched)
  PASS  disk: …/plainsick has no encryption header, so it is not encrypted
  PASS  disk: …/plainsick has an ext4 superblock but did not mount - refusing to format it
  PASS  no 'creating ext4 filesystem'
  PASS  the unmountable plain ext4 was not reformatted

== 18. the helper writes the header, then reports failure ==
        | key: … (32 bytes, not sealed)
        | disk: could not create the encrypted volume - …/halffail is left untouched
        |   initialised 4096-byte header, data at sector 2048, fingerprint 25b7721691cf0289
        |   mystic-dmcrypt: simulated failure after writing the header
        | disk: the MYSTICC1 header is on the disk; it will not be formatted
        | status: not created (header written, mapping failed)
  PASS  disk: could not create the encrypted volume - …/halffail is left untouched
  PASS  disk: the MYSTICC1 header is on the disk; it will not be formatted
  PASS  status: not created (header written, mapping failed)
  PASS  no 'creating ext4 filesystem'
  PASS  the header is still on the disk after the failure

```

The seal, all four ways it can end (scenarios 19–22), then a volume whose
header records a filesystem that is no longer visible:

```
== 19. sealed key, correct passphrase, first boot ==
passphrase:         | unsealed … -> …/ram/luks.key (50000 rounds)
        | key: unsealed … into …/ram/luks.key (RAM only)
        | crypt: initialised 4096-byte header, data at sector 2048, fingerprint 9537c58ba46bea81
        | disk: created dm-crypt volume on …/sealednew (aes-xts-plain64)
        | key: …/ram/luks.key (unsealed earlier in this boot)
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/sealednew 2048
        | disk: dm-crypt unlocked, no filesystem inside - creating one
        | fake-mount: … on /root
        | crypt: marked …/sealednew: a filesystem was created inside its mapping
        | disk: formatted ext4 inside dm-crypt and mounted on /root
        | crypt: aes-xts-plain64, key fingerprint 9537c58ba46bea81
        | status: aes-xts-plain64, key 9537c58ba46bea81
        | mounted: …
  PASS  key: unsealed … into …/ram/luks.key (RAM only)
  PASS  key: …/ram/luks.key (unsealed earlier in this boot)
  PASS  disk: created dm-crypt volume on …/sealednew (aes-xts-plain64)
  PASS  disk: formatted ext4 inside dm-crypt and mounted on /root
  PASS  mounted: …
  PASS  the unsealed key was wiped after attach

== 20. sealed key, wrong passphrase ==
passphrase: mystic-dmcrypt: that is not the lock-screen password
passphrase: mystic-dmcrypt: that is not the lock-screen password
passphrase: mystic-dmcrypt: that is not the lock-screen password
        | disk: that is not the lock-screen password (attempt 1 of 3)
        | disk: that is not the lock-screen password (attempt 2 of 3)
        | disk: that is not the lock-screen password (attempt 3 of 3)
        | disk: giving up after 3 attempts - the volume stays locked
        | disk: cannot create the encrypted volume without the key - …/sealedwrong is left untouched
        | status: locked (passphrase refused)
  PASS  disk: that is not the lock-screen password (attempt 1 of 3)
  PASS  disk: giving up after 3 attempts - the volume stays locked
  PASS  status: locked (passphrase refused)
  PASS  no 'created dm-crypt volume'
  PASS  the blank disk was not written to

== 21. sealed key, no console to prompt on ==
        | disk: … needs the passphrase, and there is no console at …
        | disk: refusing to touch …/vda, /root stays on the RAM filesystem
        | status: locked (no usable key)
  PASS  disk: … needs the passphrase, and there is no console at …
  PASS  status: locked (no usable key)
  PASS  no 'created dm-crypt volume'
  PASS  the encrypted disk was not written to

== 22. sealed key, probe mode (nobody there to type) ==
        | disk: … needs the passphrase; probe mode does not prompt
        | disk: refusing to touch …/vda, /root stays on the RAM filesystem
        | status: locked (no usable key)
  PASS  disk: … needs the passphrase; probe mode does not prompt
  PASS  status: locked (no usable key)
  PASS  the encrypted disk was not written to

== 23. the header says a filesystem was created, but no ext4 magic ==
  PASS  a freshly created volume records its filesystem in the header
        | key: … (32 bytes, not sealed)
        | crypt: host-test: mapping mystic-root from 0 14336 crypt aes-xts-plain64 <64-byte key> 0 …/marked 2048
        | disk: dm-crypt unlocked, no ext4 magic, but its header records that a
        | disk: filesystem was created in it once - refusing to format it
        | disk: nothing was written; the volume is intact
        | status: unlocked, filesystem recorded but not mounted
  PASS  disk: dm-crypt unlocked, no ext4 magic, but its header records that a
  PASS  disk: filesystem was created in it once - refusing to format it
  PASS  no 'no filesystem inside'
  PASS  the volume was not reformatted

...

106 passed, 0 failed
```

Scenarios 4, 5, 6, 10, 11, 12, 13, 15, 17, 18, 20, 21, 22 and 23 compare
`cksum` before and after: the device is byte-identical afterwards, which is
the property that matters (no `mkfs` over data). Scenario 1 uses the real
`[ -b ]` test; the other scenarios replace `disk_present()` because a Windows
build host has no block devices at all. That substitution is stated at the
top of the generated script.

### 10.3 Helper self-test

`mystic-dmcrypt selftest` runs on the host build and checks, with no invented
values: SHA-256 against the four standard vectors (empty, `"abc"`,
`"abcdbcde…nopq"`, 1,000,000 × `'a'`); HMAC-SHA256 against RFC 4231 case 2;
PBKDF2-HMAC-SHA256 against the published `password`/`salt` vectors for 1, 2
and 4096 rounds; ChaCha20 against RFC 8439 §2.4.2 (the keystream block and
the sunscreen plaintext/ciphertext pair, both reproduced independently by
OpenSSL for the same key, nonce and counter); a seal/open/fail round trip
including one flipped ciphertext byte and a tampered round count; the
hex encoding of 64 known bytes; the exact dm table string for a 2 MiB device
and for a 64 MiB device (`0 129024 crypt aes-xts-plain64 … 0 /dev/vda 2048`);
a 4 KiB header written to a file and read back through `probe()` including
both halves and the fingerprint; a blank 4 KiB file classified `blank`; an
ext4 superblock magic classified `fs`; unrecognised non-zero data classified
`unknown` (never blank); and `key_read()` accepting 32 bytes and refusing 33.
values: SHA-256 against the four standard vectors (empty, `"abc"`,
`"abcdbcde…nopq"`, 1,000,000 × `'a'`); the hex encoding of 64 known bytes; the
exact dm table string for a 2 MiB device and for a 64 MiB device
(`0 129024 crypt aes-xts-plain64 … 0 /dev/vda 2048`); a 4 KiB header written to
a file and read back through `probe()` including both halves and the
fingerprint; a blank 4 KiB file classified `blank`; an ext4 superblock magic
classified `fs`; unrecognised non-zero data classified `unknown` (never blank);
and `key_read()` accepting 32 bytes and refusing 33.

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
    ok   an all-zero 4 KiB device is blank
    ok   an ext4 superblock magic reads as fs, not blank
    ok   data that is not blank and not ext4 reads as unknown
    ok   key_read() accepts exactly 32 bytes
    ok   key_read() refuses a 33-byte file
    ok   hmac-sha256 (RFC 4231 case 2)
    ok   pbkdf2-hmac-sha256 rounds 1
    ok   pbkdf2-hmac-sha256 rounds 2
    ok   pbkdf2-hmac-sha256 rounds 4096
    ok   chacha20 keystream, counter 1
    ok   chacha20 of the RFC 8439 plaintext
    ok   the sealed key authenticates with the right passphrase
    ok   it decrypts back to the same 32 bytes
    ok   the wrong passphrase does not authenticate
    ok   one flipped ciphertext byte is detected
    ok   a tampered round count is detected
    ok   check_password_line() accepts the password it was derived from
    ok   and rejects a one-character-shorter one
    ok   0 check(s) failed
```

(The blank/fs/unknown cases were previously skipped in silence: `mkstemp()`
rewrites its template, so the second and later `mkstemp(disk)` calls failed and
their `if (fd >= 0)` bodies never ran. They go through a `tmp_open()` helper
now.) One line of that output is deliberately on stderr and left out here: the
`key_read() refuses a 33-byte file` check makes `key_read()` print
`mystic-dmcrypt: … is not exactly 32 bytes`, which is the refusal working.

### 10.4 Packaging

`tools/mkimage.py` was run into throwaway output files and the archives
inspected entry by entry — once for the tree as it ships (plaintext key), once
against a sealed copy of `stage/etc/mystic`:

```
unsealed:
  init                     mode 0100755  30207 bytes, 0 CR bytes
  sbin/mkfs.ext4           mode 0100755 108864 bytes
  sbin/mystic-dmcrypt      mode 0100755  55720 bytes
  etc/mystic/luks.key      mode 0100644     32 bytes
  etc/mystic/passwd        mode 0100644    104 bytes

sealed (a copy of stage/etc/mystic with ./tools/mkenc.sh seal applied):
  etc/mystic/luks.key.sealed  mode 0100644  112 bytes
  etc/mystic/passwd           mode 0100644  105 bytes
  (no etc/mystic/luks.key)
```

plus the three kernel modules under `lib/modules/…`. So the sealed form is
packed and the plaintext one is not — no extra step is needed beyond rebuilding
the image. Two more facts worth flagging: `mkimage.py` forces mode `0644` on
every non-executable file, so the boot half (sealed or not) is **world-readable
inside the initramfs**. Before the desktop starts, only root processes exist.
The desktop's app sandbox gives an app no `/etc` at all unless the app declared
network access, in which case it binds exactly `/etc/resolv.conf` and
`/etc/hosts` (`src/wmsrv.c:438-449`), so a uid-1000 app cannot see
`/etc/mystic/luks.key` or the seal — this is not a hole today, but it is one
bind mount away from being one. If you ever rebuild the jail with all of `/etc`,
make these files mode 0600 in the cpio first (which means teaching
`tools/mkimage.py` to preserve modes). The other fact is `/init` with zero CR
bytes, because a CRLF in a shell script is one of the documented ways to break
this project's boot.

### 10.5 What has never run

**The three dm ioctls.** `DM_DEV_CREATE`, `DM_TABLE_LOAD` and `DM_DEV_SUSPEND`
are compiled against the real `<linux/dm-ioctl.h>` (with `_Static_assert`s on the
struct sizes, 312 and 40 bytes) but they have never executed: that needs a
kernel, and this environment has no way to run one that I am allowed to use. The
first real boot is their first execution. Everything up to the ioctl boundary is
covered above; the ioctl sequence itself follows libdevmapper's and is four
calls long, and every failure prints the ioctl name and `strerror(errno)` and
exits 4, so a first-boot failure will say exactly which call failed and the boot
will say `locked (dm-crypt error)` and leave the disk alone. Treat the first
boot as the test it is.

**The termios passphrase prompt on a real console.** The prompt code
(`read_passphrase`, `tools/mkenc.sh` `cmd_unseal`) compiles for both targets,
and the simulation exercises the same function with a regular file standing in
for `/dev/console`, so the read, the retry loop and every exit code are covered.
The part that has not run is `tcgetattr`/`tcsetattr` on `/dev/console` in the VM
— turning echo off on `ttyS0`. It is nine lines and it fails soft: if the tty
calls fail, the passphrase is read with echo on and the boot still proceeds.
The one thing to check on the first sealed boot is that the typed passphrase is
not echoed to the console.

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
./tools/mkenc.sh status          # what the next boot will do (checks the key, not just the header)
./tools/mkenc.sh test            # 25 boot-path scenarios, 106 assertions, no QEMU
./tools/mkenc.sh key             # key file, length, fingerprint
./tools/mkenc.sh new-key         # replace the boot half (old disks stop opening)
./tools/mkenc.sh seal            # require the lock-screen password at boot
./tools/mkenc.sh unseal          # back to an unattended boot
./tools/mkenc.sh reset-disk      # park build/disk.img so the next boot encrypts
./tools/mkenc.sh elf <file>      # static-ELF check used by verify()

# inside the OS
mystic-dmcrypt probe /dev/vda          # exit 0 crypt | 10 blank | 11 fs | 12 unknown
mystic-dmcrypt hasfs /dev/vda          # exit 0 the header records a filesystem
mystic-dmcrypt attach mystic-root /dev/vda /etc/mystic/luks.key --table   # shape only
mystic-dmcrypt unseal /etc/mystic/luks.key.sealed /run/mystic/luks.key --prompt \
                --passwd-file /etc/mystic/passwd     # exit 5/6/7/8 on failure
mystic-dmcrypt status mystic-root
mystic-dmcrypt detach mystic-root
mystic-dmcrypt selftest
```

| build / boot message | meaning |
|---|---|
| `disk: created dm-crypt volume on /dev/vda (aes-xts-plain64)` | first boot: header written |
| `disk: dm-crypt unlocked, formatted ext4 mounted on /root` | normal encrypted boot |
| `disk: formatted ext4 inside dm-crypt and mounted on /root` | header existed, no filesystem did |
| `disk: dm-crypt unlocked, no filesystem inside - creating one` | same, in the current wording |
| `disk: dm-crypt unlocked, ext4 is there but did not mount - refusing to format it` | the volume is intact; investigate instead of reformatting |
| `disk: dm-crypt unlocked, no ext4 magic, but its header records that a` | a filesystem was created here once; it is not reformatted |
| `disk: encrypted, but the key did not unlock it` | wrong boot half; nothing was touched |
| `disk: no key at … and no sealed key at …` | no boot half in the image; an encrypted volume stays locked |
| `disk: that is not the lock-screen password (attempt 1 of 3)` | sealed key, wrong passphrase |
| `disk: giving up after 3 attempts - the volume stays locked` | sealed key, refused; nothing was touched |
| `disk: … needs the passphrase; probe mode does not prompt` | sealed image booted with `mystic.probe=1` |
| `key: unsealed … into /run/mystic/luks.key (RAM only)` | the seal opened; the file is wiped after `attach` |
| `disk: dm-crypt could not create the mapping` | device-mapper error; nothing was touched |
| `disk: … is encrypted but cannot be opened` | header present, helper/modules/parser missing; nothing was touched |
| `disk: … has no encryption header, so it is not encrypted` | plain ext4 disk from an older build |
| `disk: … has an ext4 superblock but did not mount - refusing to format it` | P0-2: a filesystem that will not mount is not reformatted |
| `disk: … did not mount and is not blank - refusing to format it` | unrecognisable disk; left untouched |
| `disk: this image has no boot key, so /dev/vda stays UNENCRYPTED` | no key at all; the old plain path, stated out loud |
| `disk: the MYSTICC1 header is on the disk; it will not be formatted` | `init` failed after writing the header |
| `crypt : aes-xts-plain64, key <hex>` | probe line: the volume that was opened |
| `crypt : locked (…)` | probe line: the volume that was refused |
