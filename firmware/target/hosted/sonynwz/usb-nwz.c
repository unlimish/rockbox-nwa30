/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* USB mass storage for the Hagoromo players (NW-A30 and later).
 *
 * The gadget itself belongs to the stock system: init brings android_usb up
 * with the mass_storage function and owns idVendor/idProduct/enable. What
 * decides whether the host sees a disk or an empty drive is a single sysfs
 * file, the backing store of LUN 0, and init.usbcfg.rc hands that one to us:
 *
 *     chown system system /sys/class/android_usb/android0/f_mass_storage/lun/file
 *     chmod 0660          /sys/class/android_usb/android0/f_mass_storage/lun/file
 *
 * which is the uid Rockbox runs as. So we do exactly what the stock
 * "on property:sys.sony.config=msc" block does, in the same order:
 *
 *     start unmount_msc1                      (umount /contents)
 *     write .../lun/file  $sys.usb.msc1       (= /emmc@contents, from init.rc)
 *
 * and its counterpart on the way out: clear the backing file, then mount the
 * partition again. Handing the host a partition that is still mounted here
 * would let both sides write to it, so the umount is not optional - if it
 * fails we leave USB mode alone rather than risk the filesystem.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"

/* 1 while the cable is supplying power. /dev/icx_power does not exist on this
 * platform, so power_input_status() cannot answer this - ask the kernel's
 * power supply class, which the stock firmware uses for the same purpose. */
#define NWZ_USB_ONLINE   "/sys/class/power_supply/usb/online"
/* LUN 0's backing store: the one knob the stock init gives us permission for */
#define NWZ_MSC_LUN_FILE \
    "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file"
/* what init.rc puts in sys.usb.msc1, i.e. the user partition */
#define NWZ_MSC_BACKING  "/emmc@contents"
/* the options the stock mount_partition uses, minus the ones mount(2) takes
 * as flags below */
#define NWZ_CONTENTS_OPTS \
    "iocharset=iso8859-1,utf8,fmask=0000,dmask=0000,shortname=mixed,discard"

#ifdef HAVE_MULTIDRIVE
void cleanup_rbhome(void);
void startup_rbhome(void);
#endif

/* Our stdout and stderr are the log file on the partition we are about to
 * release, so they would keep it busy and the umount would fail. Park them on
 * /dev/null for the duration and reopen the log afterwards - a few lost lines
 * while the host owns the disk is not a loss. */
static void stop_logging(void)
{
    fflush(stdout);
    fflush(stderr);
    int devnull = open("/dev/null", O_WRONLY);
    if(devnull < 0)
        return;
    dup2(devnull, fileno(stdout));
    dup2(devnull, fileno(stderr));
    if(devnull > fileno(stderr))
        close(devnull);
}

static void resume_logging(void)
{
    int fd = open(PIVOT_ROOT "/rockbox.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if(fd < 0)
        return;
    dup2(fd, fileno(stdout));
    dup2(fd, fileno(stderr));
    if(fd > fileno(stderr))
        close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0);
}

int usb_detect(void)
{
    int online = 0;
    if(!sysfs_get_int(NWZ_USB_ONLINE, &online))
        return USB_EXTRACTED; /* cannot tell: behave as if there is no cable */
    return online ? USB_INSERTED : USB_EXTRACTED;
}

void usb_enable(bool on)
{
    /* Nothing to do: the gadget is configured and enabled by the stock init,
     * and switching functions needs permissions we do not have. The disk is
     * handed over and taken back in disk_unmount_all()/disk_mount_all(). */
    (void)on;
}

void usb_init_device(void)
{
    /* Likewise nothing to set up. Make sure we are not leaving a stale
     * backing store from a previous run, though: if we were killed while the
     * host had the disk, the LUN would still point at it. */
    sysfs_set_string(NWZ_MSC_LUN_FILE, "");
}

/* Give the user partition to the host. Called once every thread has confirmed
 * it has stopped using the disk. Returns the number of successful unmounts. */
int disk_unmount_all(void)
{
#ifdef HAVE_MULTIDRIVE
    cleanup_rbhome();
#endif
    /* the current directory alone is enough to keep the mount busy */
    chdir("/");
    stop_logging();

    if(umount(PIVOT_ROOT) != 0)
    {
        int err = errno;
        resume_logging();
        printf("usb: cannot release %s: %s - staying out of USB mode\n",
            PIVOT_ROOT, strerror(err));
        fflush(stdout);
#ifdef HAVE_MULTIDRIVE
        startup_rbhome();
#endif
        return 0;
    }

    if(!sysfs_set_string(NWZ_MSC_LUN_FILE, NWZ_MSC_BACKING))
    {
        /* the host would see an empty drive; take the partition back rather
         * than leave the player in a state with no way to reach its files */
        disk_mount_all();
        printf("usb: cannot hand %s to the gadget\n", NWZ_MSC_BACKING);
        fflush(stdout);
        return 0;
    }
    return 1;
}

/* Take the user partition back. Called after the cable is pulled. Returns the
 * number of successful mounts. */
int disk_mount_all(void)
{
    sysfs_set_string(NWZ_MSC_LUN_FILE, "");

    int rc = mount(NWZ_MSC_BACKING, PIVOT_ROOT, "vfat",
                   MS_NOEXEC | MS_NOATIME, NWZ_CONTENTS_OPTS);
    if(rc != 0 && errno == EBUSY)
        rc = 0; /* already back, nothing to do */

    resume_logging();
    if(rc != 0)
    {
        printf("usb: cannot mount %s on %s: %s\n", NWZ_MSC_BACKING, PIVOT_ROOT,
            strerror(errno));
        fflush(stdout);
        return 0;
    }
#ifdef HAVE_MULTIDRIVE
    startup_rbhome();
#endif
    return 1;
}
