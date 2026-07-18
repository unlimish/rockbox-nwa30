#!/bin/sh

# Dualboot installer for Hagoromo-based players (NW-A30 and later).
#
# This runs as the "first script" (file 0) of the firmware upgrade, executed by
# the stock updater (icx_start_update.sh) inside the firmware-update ramdisk.
# What the stock updater has ALREADY done for us before we are called:
#   - loaded the NVP driver and validated the "fup" flag
#   - mounted the /contents (user) partition, where NW_WM_FW.UPG lives
#   - extracted file 0 (this script) and file 1 (md5.txt) from the UPG
#   - it will, on our return, umount /contents, clear the fup flag and reboot
#     REGARDLESS of our exit code, so there is no update/reboot loop to fear.
#
# The UPG we are packed into has three files:
#   0: this script
#   1: a dummy md5.txt (the updater requires a file 1 to exist)
#   2: the rockbox bootloader executable
#
# Unlike the older icx players, the main application is not SpiderApp in
# /usr/local/bin but HgrmMediaPlayerApp in the (read-only, ext4) /system, which
# we remount read-write to swap in the rockbox bootloader.

_UPDATE_FN_=`nvpstr ufn`
UPG="/contents/$_UPDATE_FN_.UPG"
OF_APP=/system/vendor/sony/bin/HgrmMediaPlayerApp
BL_TMP=/tmp/rockbox_bootloader

# log everything to the user partition so we can inspect it afterwards
exec > /contents/install_dualboot_log.txt 2>&1
set -x

# the stock application must exist (or we already backed it up on a previous run)
if [ ! -e "$OF_APP" ] && [ ! -e "$OF_APP.of" ]; then
    echo "ERROR: $OF_APP not found"
    exit 1
fi

# extract our bootloader (file index 2) to a temporary location FIRST, so that
# we never remove the stock app unless we have a valid replacement ready
mkdir -p /tmp
fwpchk -f "$UPG" -2 "$BL_TMP"
if [ "$?" != 0 ] || [ ! -s "$BL_TMP" ]; then
    echo "ERROR: failed to extract bootloader from $UPG"
    exit 1
fi

# /system is mounted read-only, make it writable
mount -o remount,rw /system
if [ "$?" != 0 ]; then
    echo "ERROR: cannot remount /system read-write"
    exit 1
fi

# back up the original application once
if [ ! -e "$OF_APP.of" ]; then
    mv "$OF_APP" "$OF_APP.of"
    if [ "$?" != 0 ]; then
        echo "ERROR: cannot back up $OF_APP"
        mount -o remount,ro /system
        exit 1
    fi
fi

# install our bootloader in place of the stock app
cp "$BL_TMP" "$OF_APP"
if [ "$?" != 0 ]; then
    echo "ERROR: cannot install bootloader, restoring stock app"
    mv "$OF_APP.of" "$OF_APP"
    mount -o remount,ro /system
    exit 1
fi
chmod 755 "$OF_APP"

sync
mount -o remount,ro /system
echo "Installation successful"
exit 0
