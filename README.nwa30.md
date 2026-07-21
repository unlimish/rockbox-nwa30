# Rockbox on the Sony NW-A30 (unofficial)

A work-in-progress Rockbox port to the Sony NW-A30 series (NW-A35/A36/A37),
Sony's "Hagoromo" platform: a MediaTek MT8127 running Linux 3.10 with a
hard-float glibc userspace.

This is **not** an official Rockbox port and is not submitted for inclusion
upstream. It is a fork of Rockbox with a new target added; everything outside
the NW-A30 target is upstream Rockbox, under its own licence (GPLv2).

## Provenance

The NW-A30 target here was written with heavy use of an AI assistant (Claude).
The upstream project asks that AI-generated contributions be explainable line by
line and does not generally accept them, which is why this lives in its own
repository rather than as a pull request. If any of it is ever useful upstream,
it should be reworked and defended by a person who understands it, not
submitted as-is.

## State

Working on the device:

- boots and runs from the user partition, replacing the stock application
- 480x800 display at 32bpp, backlight control
- all buttons (rew, ff, play/pause, volume, power) and the hold switch
- touchscreen, calibrated

Not working yet:

- **the player reboots about half a minute after Rockbox starts.** `/proc/uptime`
  restarts each time, so this is the whole machine going down rather than our
  process being killed; the stock init arms a 30 second watchdog
  (`exec /bin/wdt_ctrl 30`) which the stock application presumably keeps fed.
- audio: the mixer is mapped (see `firmware/target/hosted/sonynwz/nwzlinux-codec.c`)
  but nothing has been played yet
- FM radio: the tuner is a different part (`radio_si4708icx`) that does not
  answer the ioctls the existing driver sends, so it is left out

## Building

The toolchain is a hard-float ARMv7 one, which upstream `rockboxdev.sh` does not
build by default - the target added here (`--target=h`) does. A Docker recipe
that builds both it and the port is in `tools/docker_nwa30/`.

    docker build -f tools/docker_nwa30/Dockerfile -t rockbox-nwa30 .
    mkdir -p build/nwa30 && cd build/nwa30
    ../../tools/configure --target=235 --type=n && make

The player needs `libasound.so.2` and the bootloader font shipped alongside the
binary in `.rockbox/`, since neither can be relied on to exist on the device.

## Installing

`utils/nwztools/scripts/` has the firmware upgrade packages: `recon_hagoromo.sh`
dumps information about the device without changing it,
`install_dualboot_hagoromo.sh` installs the bootloader, and
`uninstall_dualboot_hagoromo.sh` puts the stock application back. Flashing needs
a Linux or Windows machine - `scsitool` has no macOS backend, and the official
macOS updater's kext is Intel-only.

**The stock application is renamed rather than deleted**, so the uninstall
package restores it; Sony's own firmware updater also restores everything.
