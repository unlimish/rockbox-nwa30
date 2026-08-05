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
 * Handing the user partition to the host means unmounting it, and we cannot:
 * Rockbox runs as uid 100 (system), umount(2) wants CAP_SYS_ADMIN and comes
 * back EPERM - the same wall that stops us rebooting or suspending. Asking
 * init to do it is not open to us either. It watches sys.sony.config and runs
 * unmount_msc1 when that turns "msc", but neither the property nor ctl.start
 * is writable from here; init.svc.unmount_msc1 reads back empty, proving init
 * never ran the service for us.
 *
 * The framework does the whole thing correctly on its own, provided the daemon
 * that drives it is still running - which is a question for the spare list in
 * system-nwz.c, not for this file. Driving the gadget ourselves anyway is what
 * produced the long-standing symptom of a first cable that worked and a second
 * that showed an empty drive: our writes left the framework's state machine out
 * of step with itself.
 *
 * So all this file does is stop using the disk and wait for it to go, then wait
 * for it to come back. That is exactly what the bootloader menu does, where USB
 * has always worked.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
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
/* the services init.rc defines for taking the partition away and back */
#define NWZ_SVC_UNMOUNT  "unmount_msc1"
#define NWZ_SVC_MOUNT    "mount_msc1"
/* the property init watches for the switch */
#define NWZ_USB_CONFIG_PROP "sys.sony.config"
/* how long to wait for the partition to go or come back before giving up */
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
    /* fflush only gets the lines as far as the kernel. The partition is about
     * to be handed to a host, and a log that arrives empty at the one moment it
     * is wanted has cost this port several rounds. */
    fsync(fileno(stdout));
    fsync(fileno(stderr));
    int devnull = open("/dev/null", O_WRONLY);
    if(devnull < 0)
        return;
    dup2(devnull, fileno(stdout));
    dup2(devnull, fileno(stderr));
    if(devnull > fileno(stderr))
        close(devnull);
    sync();
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

/* Resolve a /proc symlink into buf, NUL terminated. */
static bool read_link(char *buf, size_t size, const char *fmt, ...)
{
    char path[PATH_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    ssize_t n = readlink(path, buf, size - 1);
    if(n <= 0)
        return false;
    buf[n] = 0;
    return true;
}

static bool points_into_contents(const char *target)
{
    return strncmp(target, PIVOT_ROOT "/", sizeof(PIVOT_ROOT)) == 0;
}

/* When the partition does not go away the reason is almost always that
 * something still has a file open there, and it need not be us: the bootloader
 * waits for Rockbox with its own log on that partition. Name whoever it is
 * rather than leave the next person guessing. */
static char contents_users[2048];

static void note_contents_user(const char *fmt, ...)
{
    size_t used = strlen(contents_users);
    if(used >= sizeof(contents_users) - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(contents_users + used, sizeof(contents_users) - used, fmt, ap);
    va_end(ap);
}

static void collect_contents_users(void)
{
    contents_users[0] = 0;
    DIR *proc = opendir("/proc");
    if(proc == NULL)
        return;
    struct dirent *ent;
    while((ent = readdir(proc)))
    {
        if(ent->d_name[0] < '1' || ent->d_name[0] > '9')
            continue;
        /* a working directory and a root hold a mount just as firmly as an open
         * file, and neither of them shows up in /proc/<pid>/fd */
        static const char *dir_links[] = { "cwd", "root" };
        for(unsigned i = 0; i < ARRAYLEN(dir_links); i++)
        {
            char target[256];
            if(read_link(target, sizeof(target), "/proc/%s/%s", ent->d_name,
                         dir_links[i]) &&
               strncmp(target, PIVOT_ROOT, sizeof(PIVOT_ROOT) - 1) == 0)
                note_contents_user("usb:   pid %s has %s = %s\n", ent->d_name,
                    dir_links[i], target);
        }
        char fddir[sizeof("/proc//fd") + NAME_MAX];
        snprintf(fddir, sizeof(fddir), "/proc/%s/fd", ent->d_name);
        DIR *fds = opendir(fddir);
        if(fds == NULL)
            continue;
        struct dirent *fd;
        while((fd = readdir(fds)))
        {
            char target[256];
            if(read_link(target, sizeof(target), "%s/%s", fddir, fd->d_name) &&
               points_into_contents(target))
                note_contents_user("usb:   pid %s still has %s open\n",
                    ent->d_name, target);
        }
        closedir(fds);
    }
    closedir(proc);
}

/* Close everything of ours that still points at the partition, so the umount
 * does not fail with EBUSY.
 *
 * The USB screen calls font_disable_all() before acknowledging, but the player
 * still turned up with the font's glyph cache open, so do it again here: it is
 * idempotent, and by this point every thread has said it has finished with the
 * disk, which makes anything still open a cached descriptor rather than one
 * being read. */
static void release_contents_files(void)
{
    font_disable_all();

    DIR *fds = opendir("/proc/self/fd");
    if(fds == NULL)
        return;
    struct dirent *fd;
    while((fd = readdir(fds)))
    {
        char target[256];
        if(!read_link(target, sizeof(target), "/proc/self/fd/%s", fd->d_name) ||
           !points_into_contents(target))
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

/* the switch happens asynchronously, so watch the mount table for it */
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
    /* the current directory alone would be enough to keep the mount busy */
    chdir("/");
    release_contents_files();
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
