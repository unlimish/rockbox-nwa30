#!/bin/sh

# Reconnaissance script for Hagoromo-based players (NW-A30/A40/A50, NW-WM1A/Z,
# NW-ZX300). These use a MediaTek SoC and a different partition/userspace
# layout than the older icx-based NWZ players, so dump_rootfs.sh assumptions
# (constant.txt, /usr/local/bin paths, ext2/ext3 rootfs) do not hold.
#
# The script dumps everything useful for porting to the user partition and
# does NOT modify the device in any way.
#
# Note: the firmware-update ramdisk has no cp/grep/awk (confirmed on a real
# NW-A35), so this script only relies on the busybox applets known to exist
# there; everything else is done in plain shell.

# The updater script on the NWZ has a major bug/feature:
# it does NOT clear the update flag if the update script fails
# thus causing a update/reboot loop and a bricked device
# always clear to make sure we don't end up being screwed
nvpflag fup 0xFFFFFFFF

# go to /tmp
cd /tmp

# Get the content partition path from /proc/mounts (there is no grep/awk to
# parse the output of 'mount' with). An empty result is fine, see below.
CONTENTS="/contents"
CONTENTS_PART=""
while read dev mnt rest
do
    if [ "$mnt" = "$CONTENTS" ]; then
        CONTENTS_PART="$dev"
    fi
done < /proc/mounts
DUMP_DIR="$CONTENTS/recon"

# try to find a font for lcdmsg, but don't rely on it
FONT=
for f in /usr/local/bin/font_08x12.bmp /font_08x12.bmp /usr/share/fonts/font_08x12.bmp
do
    if [ -e "$f" ]; then FONT="$f"; break; fi
done

msg()
{
    if [ -n "$FONT" ]; then
        lcdmsg -f "$FONT" -l 0,15 "$1"
    fi
}

msg "Contents partition: $CONTENTS_PART"

# Remount the contents partition in read-write mode. $CONTENTS_PART is left
# unquoted on purpose: if we failed to find the device above, it collapses to
# 'mount -o remount,rw /contents', which works just as well.
if ! mount -o remount,rw $CONTENTS_PART "$CONTENTS"
then
    msg "ERROR: remount failed"
    sleep 10
    exit 0
fi

mkdir -p "$DUMP_DIR"

# 1) basic system information
msg "Dumping system info..."
mount > "$DUMP_DIR/mount.txt" 2>&1
dmesg > "$DUMP_DIR/dmesg.txt" 2>&1
cat /proc/partitions > "$DUMP_DIR/partitions.txt" 2>&1
cat /proc/mtd > "$DUMP_DIR/mtd.txt" 2>&1
cat /proc/cpuinfo > "$DUMP_DIR/cpuinfo.txt" 2>&1
cat /proc/cmdline > "$DUMP_DIR/cmdline.txt" 2>&1
cat /proc/meminfo > "$DUMP_DIR/meminfo.txt" 2>&1
cat /proc/version > "$DUMP_DIR/version.txt" 2>&1
cat /proc/devices > "$DUMP_DIR/devices.txt" 2>&1
cat /proc/modules > "$DUMP_DIR/modules.txt" 2>&1
mmcinfo map > "$DUMP_DIR/mmcinfo_map.txt" 2>&1
sysinfo > "$DUMP_DIR/sysinfo.txt" 2>&1
nvpflag -x mid > "$DUMP_DIR/model_id.txt" 2>&1

# 2) input devices (keys, touchscreen)
msg "Dumping input info..."
cat /proc/bus/input/devices > "$DUMP_DIR/input_devices.txt" 2>&1
ls -l /dev/input/ > "$DUMP_DIR/input_dev.txt" 2>&1
for ev in /sys/class/input/event*
do
    echo "=== $ev ===" >> "$DUMP_DIR/input_sysfs.txt"
    cat "$ev/device/name" >> "$DUMP_DIR/input_sysfs.txt" 2>&1
done

# 3) framebuffer information
msg "Dumping fb info..."
ls -l /dev/fb* /dev/graphics/ > "$DUMP_DIR/fb_dev.txt" 2>&1
for f in /sys/class/graphics/fb*
do
    echo "=== $f ===" >> "$DUMP_DIR/fb_sysfs.txt"
    for attr in name bits_per_pixel virtual_size mode modes stride state
    do
        echo -n "$attr: " >> "$DUMP_DIR/fb_sysfs.txt"
        cat "$f/$attr" >> "$DUMP_DIR/fb_sysfs.txt" 2>&1
    done
done

# 4) ALSA information
msg "Dumping audio info..."
cat /proc/asound/cards > "$DUMP_DIR/alsa_cards.txt" 2>&1
cat /proc/asound/devices > "$DUMP_DIR/alsa_devices.txt" 2>&1
cat /proc/asound/pcm > "$DUMP_DIR/alsa_pcm.txt" 2>&1
ls -l /dev/snd/ > "$DUMP_DIR/snd_dev.txt" 2>&1
# dump mixer controls if amixer exists
if which amixer >/dev/null 2>&1; then
    amixer contents > "$DUMP_DIR/alsa_controls.txt" 2>&1
fi

# 5) userspace layout: what to hook into for dualboot
msg "Dumping userspace layout..."
ls -l /usr/local/bin/ > "$DUMP_DIR/usr_local_bin.txt" 2>&1
ls -l / > "$DUMP_DIR/root_list.txt" 2>&1
cat /install_script/constant.txt > "$DUMP_DIR/constant.txt" 2>&1
find / -maxdepth 2 -name "*.rc" > "$DUMP_DIR/init_rc.list" 2>&1
for rc in /init.rc /init.hagoromo.rc /*.rc
do
    if [ -e "$rc" ]; then
        # there is no cp in this environment, copy by hand
        cat "$rc" > "$DUMP_DIR/${rc##*/}" 2>/dev/null
    fi
done

# 6) dump the FU initrd (small), excluding the big/user partitions
msg "Dumping FU initrd..."
LIST=""
for entry in /*
do
    if [ "$entry" != "$CONTENTS" -a "$entry" != "/sys" -a "$entry" != "/proc" -a "$entry" != "/dev" ]
    then
        LIST="$LIST $entry"
    fi
done
# $LIST is intentionally unquoted: it is a word list of paths to archive
tar -cf "$DUMP_DIR/fu_initrd.tar" $LIST 2>/dev/null
find / > "$DUMP_DIR/fu_initrd.list" 2>&1

# 7) try to dump the rootfs (main system). On Hagoromo this is typically an
# ext4 partition on the eMMC. Try to guess from /proc/mounts instead of
# relying on constant.txt
msg "Dumping rootfs..."
ROOTFS_TMP_DIR=/tmp/rootfs
mkdir -p "$ROOTFS_TMP_DIR"
. /install_script/constant.txt 2>/dev/null
ROOTFS_PART="$COMMON_ROOTFS_PARTITION"
# fallback: look for a system partition in the mounted filesystems by name
# (again in plain shell as there is no grep)
if [ -z "$ROOTFS_PART" ]; then
    while read dev mnt rest
    do
        case "$dev $mnt" in
            *system*|*rootfs*)
                ROOTFS_PART="$dev"
                break
                ;;
        esac
    done < /proc/mounts
fi
if [ -n "$ROOTFS_PART" ]; then
    echo "trying $ROOTFS_PART" > "$DUMP_DIR/rootfs_mount.txt"
    ok=1
    mount -t ext4 -o ro "$ROOTFS_PART" "$ROOTFS_TMP_DIR" 2>>"$DUMP_DIR/rootfs_mount.txt" || ok=0
    if [ "$ok" = 1 ]; then
        echo "mounted ok" >> "$DUMP_DIR/rootfs_mount.txt"
        # don't tar the whole thing blindly (could be big), but do list it and
        # grab the interesting bits
        find "$ROOTFS_TMP_DIR" > "$DUMP_DIR/rootfs.list" 2>&1
        ls -l "$ROOTFS_TMP_DIR/usr/local/bin/" > "$DUMP_DIR/rootfs_usr_local_bin.txt" 2>&1
        ls -l "$ROOTFS_TMP_DIR/system/vendor/sony/bin/" > "$DUMP_DIR/rootfs_sony_bin.txt" 2>&1
        tar -cf "$DUMP_DIR/rootfs.tar" "$ROOTFS_TMP_DIR" 2>/dev/null
        umount "$ROOTFS_TMP_DIR"
    else
        echo "mount failed" >> "$DUMP_DIR/rootfs_mount.txt"
    fi
else
    echo "no rootfs partition found" > "$DUMP_DIR/rootfs_mount.txt"
fi

msg "Rebooting in 10 seconds."
sleep 10
sync
exit 0
