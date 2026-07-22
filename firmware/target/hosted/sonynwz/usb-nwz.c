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
#include <stdlib.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "font.h"
#include "kernel.h"

/* 1 while the cable is supplying power. /dev/icx_power does not exist on this
 * platform, so power_input_status() cannot answer this - ask the kernel's
 * power supply class, which the stock firmware uses for the same purpose. */
#define NWZ_USB_ONLINE   "/sys/class/power_supply/usb/online"
/* LUN 0's backing store. init.usbcfg.rc chowns this one to "system", which is
 * the uid we run as, so it is ours to write even though the mount is not. */
#define NWZ_MSC_LUN_FILE \
    "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file"
/* what init.rc puts in sys.usb.msc1: the user partition */
#define NWZ_MSC_BACKING  "/emmc@contents"
/* the services init.rc defines for taking the partition away and back */
#define NWZ_SVC_UNMOUNT  "unmount_msc1"
#define NWZ_SVC_MOUNT    "mount_msc1"
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
static char contents_users[2048];

static void collect_contents_users(void)
{
    contents_users[0] = 0;
    size_t used = 0;
    DIR *proc = opendir("/proc");
    if(proc == NULL)
        return;
    struct dirent *ent;
    while((ent = readdir(proc)))
    {
        if(ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        /* A process's working directory and root hold a mount just as firmly
         * as an open file, and neither shows up in /proc/<pid>/fd. */
        for(unsigned k = 0; k < 2; k++)
        {
            const char *what = k ? "root" : "cwd";
            char link[sizeof("/proc//root") + NAME_MAX];
            char target[256];
            snprintf(link, sizeof(link), "/proc/%s/%s", ent->d_name, what);
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if(n <= 0)
                continue;
            target[n] = 0;
            if(strncmp(target, PIVOT_ROOT, sizeof(PIVOT_ROOT) - 1) == 0 &&
               used < sizeof(contents_users) - 1)
                used += snprintf(contents_users + used,
                                 sizeof(contents_users) - used,
                                 "usb:   pid %s has %s = %s\n",
                                 ent->d_name, what, target);
        }
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
            if(strncmp(target, PIVOT_ROOT "/", sizeof(PIVOT_ROOT)) == 0 &&
               used < sizeof(contents_users) - 1)
                used += snprintf(contents_users + used,
                                 sizeof(contents_users) - used,
                                 "usb:   pid %s still has %s open\n",
                                 ent->d_name, target);
        }
        closedir(fds);
    }
    closedir(proc);
}

/* Close everything of ours that still points at the partition, so init's
 * umount does not fail with EBUSY.
 *
 * The USB screen already calls font_disable_all() before acknowledging, but
 * the player still turned up with the font's glyph cache open, so do it again
 * here - it is idempotent, and this runs later, after every thread has said it
 * has finished with the disk. Anything left after that is a stray descriptor
 * nobody is using: by this point the threads have all acknowledged, so a file
 * still open is one that was cached rather than one being read. */
static void release_contents_files(void)
{
    font_disable_all();

    DIR *fds = opendir("/proc/self/fd");
    if(fds == NULL)
        return;
    struct dirent *fd;
    while((fd = readdir(fds)))
    {
        char link[sizeof("/proc/self/fd/") + NAME_MAX];
        char target[256];
        snprintf(link, sizeof(link), "/proc/self/fd/%s", fd->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if(n <= 0)
            continue;
        target[n] = 0;
        if(strncmp(target, PIVOT_ROOT "/", sizeof(PIVOT_ROOT)) != 0)
            continue;
        int num = atoi(fd->d_name);
        if(num > STDERR_FILENO && num != dirfd(fds))
            close(num);
    }
    closedir(fds);
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

static bool read_prop(const char *name, char *out, size_t size)
{
    int fds[2];
    if(pipe(fds) < 0)
        return false;
    pid_t pid = fork();
    if(pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if(pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl("/system/bin/getprop", "getprop", name, (char *)NULL);
        _exit(1);
    }
    close(fds[1]);
    ssize_t n = read(fds[0], out, size - 1);
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    if(n <= 0)
        return false;
    out[n] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return true;
}

static void report_prop(const char *name)
{
    char buf[64];
    if(read_prop(name, buf, sizeof(buf)))
        printf("usb:   %s = '%s'\n", name, buf);
    else
        printf("usb:   %s = (could not read)\n", name);
}



/* Read a property back, so a setprop that returned success but changed
 * nothing can be told from one that took. init only runs its actions when the
 * value actually changes, so this is the difference between "init ignored us"
 * and "we never asked". */

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
    release_contents_files();

    /* Do not touch the property, the services or the gadget. Neither
     * sys.sony.config nor ctl.start is ours to write - init.svc.unmount_msc1
     * comes back empty, so init never ran it - and the framework does the
     * whole thing correctly on its own once the daemon that drives it is left
     * running (see the spare list in system-nwz.c). Reaching in anyway got the
     * first cable working and the second showing an empty drive: our writes
     * were leaving its state machine out of step with itself.
     *
     * So all we owe it is to stop using the disk and wait. That is exactly the
     * position the bootloader menu is in, where this has always worked. */
    stop_logging();
    bool released = wait_for_contents(false);

    if(!released)
    {
        /* look before reopening the log, or we report our own file back */
        collect_contents_users();
        resume_logging();
        printf("usb: %s was not released; is the daemon that drives USB in "
            "usb_spare.txt?\n", PIVOT_ROOT);
        report_prop(NWZ_USB_CONFIG_PROP);
        /* init publishes this for every service it runs, so it says whether
         * ctl.start was accepted at all */
        report_prop("init.svc." NWZ_SVC_UNMOUNT);
        report_prop("init.svc." NWZ_SVC_MOUNT);
        report_prop("sys.usb.config");
        report_prop("sys.usb.state");
        report_prop("sys.usb.msc1");
        fputs(contents_users, stdout);
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
    /* likewise on the way out: the framework puts it back */
    bool back = wait_for_contents(true);

    resume_logging();
    if(!back)
    {
        printf("usb: %s did not come back\n", PIVOT_ROOT);
        fflush(stdout);
        return 0;
    }
    font_enable_all();
#ifdef HAVE_MULTIDRIVE
    startup_rbhome();
#endif
    return 1;
}
