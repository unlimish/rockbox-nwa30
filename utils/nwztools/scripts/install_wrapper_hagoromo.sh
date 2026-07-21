#!/bin/sh

# Install a diagnostic wrapper in front of the rockbox bootloader on
# Hagoromo players (NW-A30 and later).
#
# Same mechanics as install_dualboot_hagoromo.sh - see that script for what
# the stock updater has already done for us and why it is safe (it reboots
# and clears the update flag regardless of our exit code). The stock
# application backup (HgrmMediaPlayerApp.of) is left untouched, so
# uninstall_dualboot_hagoromo.sh still restores the player either way.
#
# The UPG we are packed into has four files:
#   0: this script
#   1: a dummy md5.txt (the updater requires a file 1 to exist)
#   2: the rockbox bootloader executable -> rockbox_bl.real
#   3: the wrapper shell script          -> HgrmMediaPlayerApp

_UPDATE_FN_=`nvpstr ufn`
UPG="/contents/$_UPDATE_FN_.UPG"
SYSTEM_PART=/emmc@android
BIN_DIR=/system/vendor/sony/bin
OF_APP="$BIN_DIR/HgrmMediaPlayerApp"
REAL_BL="$BIN_DIR/rockbox_bl.real"
we_mounted_system=0

exec > /contents/install_wrapper_log.txt 2>&1
set -x

mkdir -p /system
for fs in ext4 ext3 ext2
do
    if mount -t "$fs" "$SYSTEM_PART" /system
    then
        we_mounted_system=1
        break
    fi
done

# the stock backup must exist, otherwise we would have no way back
if [ ! -e "$OF_APP.of" ]; then
    echo "ERROR: no stock backup $OF_APP.of, refusing to touch anything"
    ls -l "$BIN_DIR"
    if [ "$we_mounted_system" = 1 ]; then
        umount /system
    fi
    exit 1
fi

release_system()
{
    sync
    if [ "$we_mounted_system" = 1 ]; then
        umount /system
    else
        mount -o remount,ro /system
    fi
}

# real bootloader first, so the wrapper never points at a missing file
fwpchk -f "$UPG" -2 "$REAL_BL"
if [ "$?" != 0 ] || [ ! -s "$REAL_BL" ]; then
    echo "ERROR: cannot install $REAL_BL"
    release_system
    exit 1
fi
chmod 755 "$REAL_BL"

fwpchk -f "$UPG" -3 "$OF_APP"
if [ "$?" != 0 ] || [ ! -s "$OF_APP" ]; then
    echo "ERROR: cannot install wrapper, restoring stock app"
    mv "$OF_APP.of" "$OF_APP"
    release_system
    exit 1
fi
chmod 755 "$OF_APP"

ls -l "$BIN_DIR/HgrmMediaPlayerApp" "$REAL_BL" "$OF_APP.of"

release_system
echo "Wrapper installation successful"
exit 0
