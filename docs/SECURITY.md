# Security model

What the OS actually does today. Nothing here is aspirational — where a
protection is missing, it says so.

## What a sandboxed app gets

A bundle (anything under `stage/usr/share/mystic/apps/<id>` or installed to
`/root/apps/<id>`) is launched by the window server with:

- **namespaces**: `unshare(CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS)`, plus
  `CLONE_NEWNET` **unless** the manifest says `network = host`, plus
  `CLONE_NEWPID` unless it says `procs = yes`
- **chroot** into `/run/mystic/jail/<id>`, which contains only:
  its own binary, the window socket, `/dev/null`, `/dev/urandom` (read-only),
  a private `/dev/shm`, `/proc`, fonts (read-only), its own writable
  `/root/appdata/<id>`, and — only when the manifest asks — `/etc/ssl/certs`
  or `/dev/snd`
- **uid drop** to a dedicated unprivileged uid, with `setgroups(0)` so no
  supplementary group leaks capability
- **descriptor hygiene**: every inherited fd is closed before `exec`, so the
  framebuffer, the keyboard and the lock-screen PIN are not reachable

Privileged work (launching another app, installing a package, opening a file
dialog, changing the theme) is requested by writing a file the desktop polls.
A sandboxed app therefore never needs to fork or to hold a privileged handle.

## Manifest flags and what they mean

| flag | effect | why an app needs it |
|---|---|---|
| `network = host` | keeps the host network namespace, binds `resolv.conf` + `hosts` + the CA bundle | the browser |
| `audio = yes` | binds `/dev/snd` | the Clock's alarm, the player |
| `procs = yes` | skips `CLONE_NEWPID` and binds the host `/proc` read-only | Sysmon and Monitor, which list processes |

Each of these widens the attack surface on purpose. An app without them cannot
reach the network, cannot make sound, and cannot see other processes.

## What is NOT protected

- **The in-desktop apps are not sandboxed.** Files, Settings, Monitor, Notes,
  Store, Calculator and System are compiled into the compositor and run with
  its privileges; a bug in one is a bug in the desktop. The terminal is in the
  same position: `forkpty()` runs `/bin/sh` as root with `/root` as HOME.
- **Apps share nothing but a uid pool** — the per-app uid and per-app `/dev/shm`
  are the newest change; a shared `SANDBOX_UID` was the previous state.
- **No disk encryption.** `/root` is a plain ext4 volume. A design for
  dm-crypt with a passphrase-unlocked key file exists in
  [ENCRYPTION.md](ENCRYPTION.md) and is **not** in the image.
- **Packages are integrity-checked, not authenticated.** The index carries a
  SHA-256 per file and the transfer is verified against it, but a forged index
  would be believed: Ed25519 verification is implemented
  ([SIGNING.md](SIGNING.md)) and not yet enforced end to end.
- **No multi-user support.** One account, one password, one home.
- **No MAC, no seccomp filters.** When an app is jailed it relies on the
  namespace and uid boundaries, not on syscall filtering.

## Reporting

Open an issue with the reproduction. A sandbox escape that a reader can follow
step by step is more useful than a severity label.
