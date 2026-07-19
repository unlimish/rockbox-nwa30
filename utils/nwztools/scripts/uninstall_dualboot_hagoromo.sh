#!/bin/sh

# Recovery/uninstall script for Hagoromo-based players (NW-A30 and later).
#
# It restores the stock application that install_dualboot_hagoromo.sh moved out
# of the way, undoing the dualboot installation. Use this if the player no
# longer boots (e.g. the rockbox bootloader crashes and the system restarts it
# in a loop): the firmware upgrade environment runs from its own ramdisk and
# never executes the broken application, so this always gets you back to stock.
#
# The UPG contains two files:
#   0: this script
#   1: a dummy md5.txt (the stock updater requires a file 1 to exist)
#
# Note: this environment has no cp/grep, so we only use mv/mount/ls/chmod.

SYSTEM_PART=/emmc@android
OF_APP=/system/vendor/sony/bin/HgrmMediaPlayerApp
we_mounted_system=0

# log to the user partition so the result can be inspected afterwards
exec > /contents/uninstall_dualboot_log.txt 2>&1
set -x

# mount the system partition (it is not mounted in this environment)
mkdir -p /system
for fs in ext4 ext3 ext2
do
    if mount -t "$fs" "$SYSTEM_PART" /system
    then
        we_mounted_system=1
        break
    fi
done

# we need the backup made at install time
if [ ! -e "$OF_APP.of" ]; then
    echo "ERROR: no backup $OF_APP.of to restore"
    ls -l /system/vendor/sony/bin
    if [ "$we_mounted_system" = 1 ]; then
        umount /system
    fi
    exit 1
fi

# put the stock application back (this overwrites the rockbox bootloader)
mv "$OF_APP.of" "$OF_APP"
if [ "$?" != 0 ]; then
    echo "ERROR: cannot restore $OF_APP"
    if [ "$we_mounted_system" = 1 ]; then
        umount /system
    fi
    exit 1
fi
chmod 755 "$OF_APP"

sync
if [ "$we_mounted_system" = 1 ]; then
    umount /system
fi

echo "Restore successful"
exit 0
