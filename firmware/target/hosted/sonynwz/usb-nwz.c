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
 * Handing the user partition to the host means unmounting it first, and we
 * cannot do that ourselves: Rockbox runs as uid 100 (system), umount(2) needs
 * CAP_SYS_ADMIN, and it comes back EPERM - the same wall that stops us
 * rebooting or suspending. The stock firmware does not do it directly either.
 * It asks init, which is root, by setting a property:
 *
 *     on property:sys.sony.config=msc
 *         start unmount_msc1                       (umount /contents)
 *         write .../f_mass_storage/lun/file  $sys.usb.msc1   (= /emmc@contents)
 *         ... switch the gadget to mass_storage,adb
 *
 *     on property:sys.sony.config=adb
 *         write .../f_mass_storage/lun/file  ""
 *         ... switch back
 *         start mount_msc1                         (mount /contents again)
 *
 * So do the same. The framework normally sets that property in response to the
 * cable, but the service that would is one of those Rockbox freezes to stop
 * the machine restarting under it - which is why the player has been turning
 * up as a pair of empty drives - so we set it ourselves and wait for init to
 * finish the job.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "kernel.h"

/* 1 while the cable is supplying power. /dev/icx_power does not exist on this
 * platform, so power_input_status() cannot answer this - ask the kernel's
 * power supply class, which the stock firmware uses for the same purpose. */
#define NWZ_USB_ONLINE   "/sys/class/power_supply/usb/online"
/* the property init watches, and the two values it switches on */
#define NWZ_USB_CONFIG_PROP "sys.sony.config"
#define NWZ_USB_CONFIG_MSC  "msc"
#define NWZ_USB_CONFIG_ADB  "adb"
/* how long to give init to unmount or remount before giving up */
#define NWZ_USB_TIMEOUT_TICKS (5 * HZ)

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

/* init unmounts on our behalf, so when that fails the reason is almost
 * always that something still has a file open there - and it need not be us:
 * the bootloader waits for Rockbox with its own log on the partition. Name
 * whoever it is rather than leave the next person guessing. */
static void report_contents_users(void)
{
    DIR *proc = opendir("/proc");
    if(proc == NULL)
        return;
    struct dirent *ent;
    while((ent = readdir(proc)))
    {
        if(ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        char fddir[sizeof("/proc//fd") + NAME_MAX];
        snprintf(fddir, sizeof(fddir), "/proc/%s/fd", ent->d_name);
        DIR *fds = opendir(fddir);
        if(fds == NULL)
            continue;
        struct dirent *fd;
        while((fd = readdir(fds)))
        {
            char link[sizeof(fddir) + 1 + NAME_MAX];
            char target[256];
            snprintf(link, sizeof(link), "%s/%s", fddir, fd->d_name);
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if(n <= 0)
                continue;
            target[n] = 0;
            if(strncmp(target, PIVOT_ROOT "/", sizeof(PIVOT_ROOT)) == 0)
                printf("usb:   pid %s still has %s open\n", ent->d_name, target);
        }
        closedir(fds);
    }
    closedir(proc);
}

static bool contents_mounted(void)
{
    FILE *f = fopen("/proc/mounts", "re");
    if(f == NULL)
        return true; /* cannot tell; assume it is there and do not act */
    char line[512];
    bool found = false;
    while(!found && fgets(line, sizeof(line), f))
        found = strstr(line, " " PIVOT_ROOT " ") != NULL;
    fclose(f);
    return found;
}

/* Ask init to switch the USB configuration. Returns false if we could not
 * even run setprop. */
static bool set_usb_config(const char *value)
{
    pid_t pid = fork();
    if(pid < 0)
        return false;
    if(pid == 0)
    {
        execl("/system/bin/setprop", "setprop", NWZ_USB_CONFIG_PROP, value,
              (char *)NULL);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return status == 0;
}

/* init does the work asynchronously, so wait for the mount table to show it */
static bool wait_for_contents(bool want_mounted)
{
    long deadline = current_tick + NWZ_USB_TIMEOUT_TICKS;
    while(TIME_BEFORE(current_tick, deadline))
    {
        if(contents_mounted() == want_mounted)
            return true;
        sleep(HZ / 10);
    }
    return contents_mounted() == want_mounted;
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
    /* The gadget is brought up and switched by init; see disk_unmount_all()
     * and disk_mount_all(), which is where we ask it to. */
    (void)on;
}

void usb_init_device(void)
{
}

/* Give the user partition to the host. Called once every thread has confirmed
 * it has stopped using the disk. Returns the number of successful unmounts. */
int disk_unmount_all(void)
{
#ifdef HAVE_MULTIDRIVE
    cleanup_rbhome();
#endif
    /* the current directory alone would keep the mount busy for init */
    chdir("/");
    stop_logging();

    bool asked = set_usb_config(NWZ_USB_CONFIG_MSC);
    bool released = asked && wait_for_contents(false);

    if(!released)
    {
        resume_logging();
        printf("usb: init did not release %s (setprop %s)\n", PIVOT_ROOT,
            asked ? "ok" : "failed");
        report_contents_users();
        fflush(stdout);
#ifdef HAVE_MULTIDRIVE
        startup_rbhome();
#endif
        return 0;
    }
    return 1;
}

/* Take the user partition back. Called after the cable is pulled. Returns the
 * number of successful mounts. */
int disk_mount_all(void)
{
    bool asked = set_usb_config(NWZ_USB_CONFIG_ADB);
    bool back = asked && wait_for_contents(true);

    resume_logging();
    if(!back)
    {
        printf("usb: init did not give %s back (setprop %s)\n", PIVOT_ROOT,
            asked ? "ok" : "failed");
        fflush(stdout);
        return 0;
    }
#ifdef HAVE_MULTIDRIVE
    startup_rbhome();
#endif
    return 1;
}
