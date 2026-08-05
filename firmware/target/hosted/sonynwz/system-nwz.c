/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2016 Amaury Pouly
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

#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <limits.h>

#include "system.h"
#include "lcd.h"
#include "font.h"
#include "system.h"
#include "backlight-target.h"
#include "button.h"
#include "adc.h"
#include "power.h"
#include "mv.h"
#include "power-nwz.h"
#include <backtrace.h>
#include "version.h"

#include "logf.h"

static const char **kern_mod_list;
bool os_file_exists(const char *ospath);

int nwz_power_shutdown(void);
#if !defined(BOOTLOADER)
void nwz_thaw_framework(void);
#endif

#ifdef SONY_NWA30
#define SINGLE_INSTANCE_LOCK    "/tmp/rockbox.lock"
#define USB_SPARE_LIST_FILE     "/contents/.rockbox/usb_spare.txt"
#define BATTERY_SYSFS_DIR       "/sys/class/power_supply/battery"
/* the framework core gives this thread to every one of its daemons, so it
 * identifies the framework rather than any single service */
#define HANG_CHECKER_THREAD     "fr_job"

/* Read the first line of a file, without its newline. Most of what this port
 * asks the kernel about is a single short line in /proc or /sys. */
static bool read_first_line(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "re");
    if(f == NULL)
        return false;
    bool ok = fgets(buf, size, f) != NULL;
    fclose(f);
    if(!ok)
        return false;
    buf[strcspn(buf, "\r\n")] = 0;
    return true;
}
#endif

void power_off(void)
{
#ifdef SONY_NWA30
    /* The icx players exit back to the stock firmware, but here exiting only
     * reaches our own bootloader, which starts Rockbox again - so on this
     * player leaving is not turning off. */
    nwz_power_shutdown();
#endif
    exit(0);
}

static void compute_kern_mod_list(void)
{
    /* create empty list */
    kern_mod_list = malloc(sizeof(const char **));
    kern_mod_list[0] = NULL;
    /* read from proc file system */
    FILE *f = fopen("/proc/modules", "re");
    if(f == NULL)
    {
        printf("Cannot open /proc/modules");
        return;
    }
    for(int i = 0;; i++)
    {
        /* the last entry of the list points to NULL so getline() will allocate
         * some memory */
        size_t n;
        if(getline((char **)&kern_mod_list[i], &n, f) < 0)
        {
            /* make sure last entry is NULL and stop */
            kern_mod_list[i] = NULL;
            break;
        }
        /* grow array */
        kern_mod_list = realloc(kern_mod_list, (i + 2) * sizeof(const char **));
        /* and fill last entry with NULL */
        kern_mod_list[i + 1] = NULL;
        /* parse line to only keep module name */
        char *p = strchr(kern_mod_list[i], ' ');
        if(p != NULL)
            *p = 0; /* stop at first blank */
    }
    fclose(f);
}

static void print_kern_mod_list(void)
{
    printf("Kernel modules:\n");
    const char **p = kern_mod_list;
    while(*p)
        printf("  %s\n", *p++);
}

/* The stock application is restarted every half minute or so, and Rockbox
 * outlives that by running in its own session, so the copy started in our place
 * can find one already running. Two of them fight over the framebuffer and the
 * input devices. A lock beats looking for our own name in /proc: the kernel
 * drops it exactly when the process dies, so it cannot go stale. */
#if defined(SONY_NWA30) && !defined(BOOTLOADER)
static void claim_single_instance(void)
{
    int fd = open(SINGLE_INSTANCE_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if(fd < 0)
        return; /* cannot tell - carry on rather than refuse to start */
    if(flock(fd, LOCK_EX | LOCK_NB) < 0)
    {
        printf("another rockbox is already running, exiting\n");
        fflush(stdout);
        exit(0);
    }
    /* fd is deliberately leaked: the lock must outlast this function */
}
#endif

#ifdef SONY_NWA30
/* The kernel command line records why the machine last came up, which tells a
 * watchdog timeout apart from an orderly restart. Sony's icx_pm_helper driver
 * publishes the same thing split into flags, which is less ambiguous - the
 * names come from drivers/power/icx_pm_helper.c in their kernel source. */
static void print_boot_reason(void)
{
    char line[1024];
    if(read_first_line("/proc/cmdline", line, sizeof(line)))
    {
        const char *reason = strstr(line, "bootreason=");
        if(reason != NULL)
        {
            reason += sizeof("bootreason=") - 1;
            printf("Boot reason: %.*s\n", (int)strcspn(reason, " "), reason);
        }
    }

    static const char *flags[] =
    {
        "boot_powerkey", "boot_reset", "boot_reboot",
        "boot_deadbat", "boot_wdt", "boot_thermal", "boot_option",
    };
    for(unsigned i = 0; i < ARRAYLEN(flags); i++)
    {
        char path[PATH_MAX], value[64];
        snprintf(path, sizeof(path),
            "/sys/devices/platform/icx_pm_helper/%s", flags[i]);
        if(read_first_line(path, value, sizeof(value)))
            printf("  %-14s %s\n", flags[i], value);
    }
}

static void print_file_tail(const char *path, const char *what, long tail_bytes)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0)
        return;
    if(tail_bytes > 0)
    {
        off_t end = lseek(fd, 0, SEEK_END);
        lseek(fd, end > tail_bytes ? end - tail_bytes : 0, SEEK_SET);
    }
    printf("--- %s (%s) ---\n", what, path);
    char buf[512];
    ssize_t n;
    while((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = 0;
        fputs(buf, stdout);
    }
    printf("\n--- end of %s ---\n", what);
    close(fd);
}

/* A watchdog timeout, a panic and a deliberate restart each say so in their own
 * words in the previous boot's log, which is the only account of a reboot that
 * takes the machine down with us. */
static void print_restart_evidence(void)
{
    print_file_tail("/proc/wdk", "watchdog state", 0);
    /* the previous kernel log, under the two names it goes by here */
    print_file_tail("/proc/last_kmsg", "previous kernel log", 2048);
    print_file_tail("/sys/fs/pstore/console-ramoops", "previous console", 2048);
}

/* /contents is the user partition and /contents_ext the memory card. Both are
 * matched with their trailing space, so that /contents_ext is not swallowed by
 * the test for /contents - the card's absence is what a database missing half
 * its music looks like from here. /contents being noexec is likewise what
 * dlopen() failing on a codec looks like. */
static void print_mount_info(void)
{
    FILE *f = fopen("/proc/mounts", "re");
    if(f == NULL)
        return;
    printf("--- mounts (/contents, /contents_ext, /tmp) ---\n");
    char line[512];
    while(fgets(line, sizeof(line), f))
    {
        if(strstr(line, " /contents ") || strstr(line, " /contents_ext ") ||
           strstr(line, " /tmp "))
            fputs(line, stdout);
    }
    printf("--- end of mounts ---\n");
    fclose(f);
}

/* What the tuner presents itself as, if anything.
 *
 * radio-nwz.c speaks the icx ioctls the older players use and gets ENOTTY here,
 * so the FM radio is left out. Sony's radio_si4708icx module has no source in
 * their kernel release, which leaves the question of what interface it does
 * offer - and a V4L2 node would be a far easier one to drive than a private
 * ioctl set. Cheap to ask, so ask on every boot until it is answered. */
/* Questions about the device that are answered by looking, written to their own
 * small file as well as to the log.
 *
 * The log is the natural place for this, but it lives on the partition that
 * gets handed to a USB host, and it has arrived empty at exactly the moment it
 * was wanted more than once. This file is opened, written and closed in one go
 * at startup, so there is nothing of it left in a buffer to lose. */
#define PROBE_REPORT_FILE "/contents/nwa30_probe.txt"

static void probe_device(void)
{
    static const char *tuner_nodes[] =
    {
        "/dev/radio0", "/dev/radio", "/dev/icx_radio", "/dev/si4708",
        /* The other way in. Sony's board file
         * (arch/arm/mach-mt8590/icx_radio_i2c_devs.c) puts the Si4708 on i2c
         * bus 2 at address 0x10, with GPIO278 as its reset - and the Si470x
         * register map is public, so the chip can be driven without their
         * driver, provided i2c-dev is there and lets us at the bus. */
        "/dev/i2c-0", "/dev/i2c-1", "/dev/i2c-2", "/dev/i2c-3", "/dev/i2c/2",
    };
    static const char *writable[] =
    {
        "/sys/devices/platform/icx_pm_helper/force_power_off",
    };

    FILE *f = fopen(PROBE_REPORT_FILE, "we");

    for(unsigned i = 0; i < ARRAYLEN(tuner_nodes); i++)
    {
        const char *path = tuner_nodes[i];
        const char *state;
        if(access(path, F_OK) != 0)
            state = "absent";
        else if(access(path, R_OK | W_OK) == 0)
            state = "present, ours to open";
        else
            state = "present, NOT ours to open";
        printf("probe: %-16s %s\n", path, state);
        if(f)
            fprintf(f, "%-48s %s\n", path, state);
    }
    for(unsigned i = 0; i < ARRAYLEN(writable); i++)
    {
        const char *path = writable[i];
        const char *state = access(path, F_OK) != 0 ? "absent" :
                            access(path, W_OK) == 0 ? "writable" :
                                                      "read-only for us";
        printf("probe: %s %s\n", path, state);
        if(f)
            fprintf(f, "%-48s %s\n", path, state);
    }

    if(f)
    {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    fflush(stdout);
}

/* We take a percentage from "capacity" and have never read the rest. Whether
 * "current_now" is there decides whether power draw can be measured at all. */
static void print_battery_nodes(void)
{
    DIR *dir = opendir(BATTERY_SYSFS_DIR);
    if(dir == NULL)
    {
        printf("battery: cannot open %s (%s)\n", BATTERY_SYSFS_DIR,
            strerror(errno));
        return;
    }
    printf("--- %s ---\n", BATTERY_SYSFS_DIR);
    struct dirent *entry;
    while((entry = readdir(dir)) != NULL)
    {
        if(entry->d_name[0] == '.')
            continue;
        char path[PATH_MAX], value[64];
        snprintf(path, sizeof(path), "%s/%s", BATTERY_SYSFS_DIR, entry->d_name);
        if(read_first_line(path, value, sizeof(value)))
            printf("  %-24s %s\n", entry->d_name, value);
        else
            printf("  %-24s (unreadable: %s)\n", entry->d_name, strerror(errno));
    }
    printf("--- end of battery ---\n");
    closedir(dir);
    fflush(stdout);
}

/* Nothing to pet: the watchdog on this platform is kicked by the kernel. */
void nwz_watchdog_pet(void)
{
}
#endif

/* Rising across a restart means only our process was replaced; back to zero
 * means the whole player rebooted. */
static void print_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "re");
    if(f == NULL)
        return;
    double up;
    if(fscanf(f, "%lf", &up) == 1)
        printf("System uptime: %.1f s\n", up);
    fclose(f);
}

/* We run in place of the stock application while the rest of the stock
 * userspace keeps running, so this is how we find both what to log and what to
 * stop. The command line buffer holds a whole hagodaemon invocation: truncating
 * it once hid the very service names we meant to spare, and they got frozen. */
static void for_each_process(void (*fn)(int pid, const char *cmdline))
{
    DIR *proc = opendir("/proc");
    if(proc == NULL)
        return;
    struct dirent *entry;
    while((entry = readdir(proc)))
    {
        if(entry->d_name[0] < '1' || entry->d_name[0] > '9')
            continue; /* only numeric entries are processes */
        char path[sizeof("/proc//cmdline") + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE *f = fopen(path, "re");
        if(f == NULL)
            continue;
        char cmdline[512];
        size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
        fclose(f);
        if(n == 0)
            continue; /* kernel thread, no cmdline */
        /* cmdline is NUL-separated argv, turn the separators into spaces */
        for(size_t i = 0; i < n; i++)
            if(cmdline[i] == 0)
                cmdline[i] = ' ';
        cmdline[n] = 0;
        fn(atoi(entry->d_name), cmdline);
    }
    closedir(proc);
}

static void print_one_process(int pid, const char *cmdline)
{
    printf("  %5d %s\n", pid, cmdline);
}

static void print_proc_list(void)
{
    printf("Processes:\n");
    for_each_process(print_one_process);
}

/* libpstcore.so watches every application's main thread and restarts the
 * machine when it decides one is stuck:
 *
 *     Hang detected! pid=%s
 *     Application(pid=%s) main thread is freezed, reboot the system...
 *
 * The stock application reports in over the Sony IPC framework and we do not,
 * so the countdown is always running against us. Freezing beats killing: init
 * would start a killed daemon again and the same countdown would begin, while a
 * stopped one stays in the process table and never reaches its timeout.
 *
 * Everything stopped goes on this list, because a frozen framework outlives us
 * - the bootloader we return to is a separate process and knows nothing of what
 * we stopped, so the services that hand the user partition to the USB gadget
 * would stay stopped and the player would come up as an empty drive. */
#if !defined(BOOTLOADER)
#define MAX_FROZEN 64
static int frozen_pids[MAX_FROZEN];
static int nr_frozen;

static void freeze_process(int pid)
{
    for(int i = 0; i < nr_frozen; i++)
        if(frozen_pids[i] == pid)
            return; /* already stopped, and already on the list */
    if(kill(pid, SIGSTOP) != 0)
        return;
    if(nr_frozen < MAX_FROZEN)
        frozen_pids[nr_frozen++] = pid;
    else
        printf("warning: more than %d frozen processes, %d will stay stopped\n",
            MAX_FROZEN, pid);
}

void nwz_thaw_framework(void)
{
    for(int i = 0; i < nr_frozen; i++)
        kill(frozen_pids[i], SIGCONT);
    if(nr_frozen > 0)
    {
        printf("resumed %d framework processes\n", nr_frozen);
        fflush(stdout);
    }
    nr_frozen = 0;
}

/* Answers on the first match. A process gets several threads by the same name,
 * and reporting each of them used to record its pid over and over, enough to
 * overflow the frozen table and leave real processes stopped for good. */
static bool process_has_thread(int pid, const char *thread)
{
    char dir[32];
    snprintf(dir, sizeof(dir), "/proc/%d/task", pid);
    DIR *task = opendir(dir);
    if(task == NULL)
        return false;
    bool found = false;
    struct dirent *tid;
    while(!found && (tid = readdir(task)))
    {
        if(tid->d_name[0] < '1' || tid->d_name[0] > '9')
            continue;
        char path[sizeof(dir) + 1 + NAME_MAX + sizeof("/comm")], comm[32];
        snprintf(path, sizeof(path), "%s/%s/comm", dir, tid->d_name);
        found = read_first_line(path, comm, sizeof(comm)) &&
                strcmp(comm, thread) == 0;
    }
    closedir(task);
    return found;
}

/* Names read once from USB_SPARE_LIST_FILE, one per line, added to the built-in
 * list below. It exists so another candidate can be tried without reflashing. */
static char extra_spared_services[512];

static void load_extra_spared_services(void)
{
    static bool loaded = false;
    if(loaded)
        return;
    loaded = true;
    FILE *f = fopen(USB_SPARE_LIST_FILE, "re");
    if(f == NULL)
        return;
    size_t n = fread(extra_spared_services, 1,
                     sizeof(extra_spared_services) - 1, f);
    fclose(f);
    extra_spared_services[n] = 0;
    printf("usb: also sparing services from usb_spare.txt:\n%s\n",
        extra_spared_services);
    fflush(stdout);
}

/* One name per line, '#' comments out a line, trailing blanks are ignored. */
static bool listed_in(const char *list, const char *cmdline)
{
    for(const char *line = list; *line; )
    {
        size_t len = strcspn(line, "\r\n");
        char name[64];
        while(len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
            len--;
        if(len > 0 && len < sizeof(name) && *line != '#')
        {
            memcpy(name, line, len);
            name[len] = 0;
            if(strstr(cmdline, name) != NULL)
                return true;
        }
        line += strcspn(line, "\r\n");
        line += strspn(line, "\r\n");
    }
    return false;
}

/* The whole chain that carries a cable insertion has to stay awake, because we
 * cannot hand the partition to a host ourselves: sys.sony.config and ctl.start
 * are both refused for uid system, and init.svc.unmount_msc1 reads back empty
 * to prove init never ran it. Freezing any one of these left the player showing
 * up as an empty drive. PathMgrServiceFw is deliberately absent - sparing that
 * one makes the machine restart. */
static bool service_is_needed(const char *cmdline)
{
    static const char *needed[] =
    {
        "WMPortService",             /* the connector this player has */
        "EventRouter",               /* delivers the insertion to the rest */
        "FuncMgrServiceFw",          /* picks the mode to switch into */
        "ConnMgrServiceFw",          /* and drives the switch */
        "UsbMgrServiceFw",           /* USB itself */
        "UsbHostConnectionService",  /* and the connection state it acts on */
        "StorageMgrServiceFw",       /* the storage behind the drive */
    };
    for(unsigned i = 0; i < ARRAYLEN(needed); i++)
        if(strstr(cmdline, needed[i]) != NULL)
            return true;
    load_extra_spared_services();
    return listed_in(extra_spared_services, cmdline);
}

static void freeze_hang_checker(int pid, const char *cmdline)
{
    if(service_is_needed(cmdline))
    {
        printf("leaving alone: pid %d %s\n", pid, cmdline);
        return;
    }
    if(!process_has_thread(pid, HANG_CHECKER_THREAD))
        return;
    printf("thread '%s' belongs to pid %d: %s\nfreezing it\n",
        HANG_CHECKER_THREAD, pid, cmdline);
    freeze_process(pid);
}
#endif

#if !defined(BOOTLOADER)
static void freeze_app_manager(int pid, const char *cmdline)
{
    if(strstr(cmdline, "appmgrservice") != NULL)
    {
        printf("freezing app manager (pid %d)\n", pid);
        freeze_process(pid);
    }
}
#endif

/* Nothing tells the stock boot splash that boot is over, so it keeps drawing
 * over our screen. The stock application stops it too, just politely. */
static void kill_boot_animation(int pid, const char *cmdline)
{
    if(strstr(cmdline, "icx_bootanimation") != NULL)
    {
        printf("stopping boot animation (pid %d)\n", pid);
        kill(pid, SIGKILL);
    }
}

/* to make thread-internal.h happy */
uintptr_t *stackbegin;
uintptr_t *stackend;

static void dump_proc_map(void)
{
    const char *file = "/proc/self/maps";
    printf("Dumping %s...\n", file);
    FILE *f = fopen(file, "re");
    if(f == NULL)
    {
        perror("Cannot open file");
        return;
    }
    while(true)
    {
        char *line = NULL;
        size_t n;
        if(getline(&line, &n, f) < 0)
            break;
        printf("> %s", line);
        free(line);
    }
    fclose(f);
}

static void nwz_sig_handler(int sig, siginfo_t *siginfo, void *context)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

    /* SIGTERM is not a fault: it is the system asking us to quit, which it
     * does on this platform when it wants the player back (to enter USB mass
     * storage mode, for instance). Showing a backtrace and rebooting for it is
     * both alarming and wrong, so just leave quietly. */
    if(sig == SIGTERM)
    {
        printf("terminated by the system\n");
        fflush(stdout);
        exit(0);
    }

    /* dump process maps to log file to ease debugging
     * will also print crash info to the log */
    dump_proc_map();

    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();

    /* get context info */
    ucontext_t *uc = (ucontext_t *)context;
    unsigned long pc = uc->uc_mcontext.arm_pc;
    unsigned long sp = uc->uc_mcontext.arm_sp;

    printf("%s at %08x\n", strsignal(sig), (unsigned int)pc);
    lcd_putsf(0, line++, "%s at %08x", strsignal(sig), pc);

    if(sig == SIGILL || sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP)
    {
        printf("address 0x%08x\n", (unsigned int)siginfo->si_addr);
        lcd_putsf(0, line++, "address 0x%08x", siginfo->si_addr);
    }

    if(!triggered)
    {
        triggered = true;
        rb_backtrace(pc, sp, &line);
    }

#ifdef ROCKBOX_HAS_LOGF
    lcd_putsf(0, line++, "logf:");
    logf_panic_dump(&line);
#endif

    lcd_update();

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}

void system_init(void)
{
    int *s;
    /* Our stdout is a file on the player and would be block buffered, losing
     * everything still in the buffer when a signal we cannot handle kills us -
     * which reads as if we never got that far. */
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef SONY_NWA30
    /* First line of every log: a stale build is indistinguishable from a broken
     * one otherwise, and each mix-up costs a flash and test cycle. */
    printf("build: %s (%s)\n", rbversion,
#if defined(BOOTLOADER)
        "bootloader"
#else
        "main"
#endif
        );
    fflush(stdout);
#endif
#if defined(SONY_NWA30) && !defined(BOOTLOADER)
    claim_single_instance();
#endif
    /* fake stack, to make thread-internal.h happy */
    stackbegin = stackend = (uintptr_t*)&s;
    /* catch some signals for easier debugging */
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &nwz_sig_handler;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#ifdef SONY_NWA30
    /* The stock userspace sends SIGTERM a little while after we start whether
     * or not the player is in use, and honouring it loops: we quit, the system
     * starts the application we replaced, and that is us again. We do not get
     * to catch the SIGKILL if it escalates. */
    signal(SIGTERM, SIG_IGN);
#else
    /* not a fault, but we want to leave cleanly rather than be killed
     * mid-write, see nwz_sig_handler() */
    sigaction(SIGTERM, &sa, NULL);
#endif
    compute_kern_mod_list();
    print_kern_mod_list();
    print_uptime();
#ifdef SONY_NWA30
    print_boot_reason();
    print_restart_evidence();
    print_mount_info();
    print_battery_nodes();
    probe_device();

#endif
    print_proc_list();
    for_each_process(kill_boot_animation);
#if !defined(BOOTLOADER)
    for_each_process(freeze_app_manager);
    for_each_process(freeze_hang_checker);
    /* Whatever we stopped has to be started again when we go, however we go:
     * we hand control back to the bootloader, and a framework left frozen
     * there cannot give the user partition to the USB gadget - the player
     * appears as an empty drive and the only way out is a hard power off. */
    atexit(nwz_thaw_framework);
#endif
    /* some init not done on hosted targets */
    adc_init();
    power_init();
}


void system_reboot(void)
{
#ifdef SONY_NWA30
    /* reboot(2) wants CAP_SYS_BOOT and we run as "system", so a real restart
     * is out of reach. Quitting is the next best thing and is what a reboot is
     * wanted for here anyway: the bootloader shows its menu again when Rockbox
     * returns, so this lands where the user was heading. Do not power off on
     * the way - that suspends, and the player then has to be woken to get
     * anywhere. The framework is thawed by the atexit handler. */
    exit(0);
#else
    power_off();
#endif
}

#ifdef HAVE_BUTTON_DATA
#define IF_DATA(data) data
#else
#define IF_DATA(data)
#endif
void system_exception_wait(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    /* wait until button press and release */
    IF_DATA(int data);
    while(button_read_device(IF_DATA(&data)) != 0) {}
    while(button_read_device(IF_DATA(&data)) == 0) {}
    while(button_read_device(IF_DATA(&data)) != 0) {}
    while(button_read_device(IF_DATA(&data)) == 0) {}
}

int hostfs_init(void)
{
    return 0;
}

int hostfs_flush(void)
{
    sync();
    return 0;
}

const char **nwz_get_kernel_module_list(void)
{
    return kern_mod_list;
}

bool nwz_is_kernel_module_loaded(const char *name)
{
    const char **p = kern_mod_list;
    while(*p)
        if(strcmp(*p++, name) == 0)
            return true;
    return false;
}

#ifdef CONFIG_STORAGE_MULTI
int hostfs_driver_type(int drive)
{
    return drive > 0 ? STORAGE_SD_NUM : STORAGE_HOSTFS_NUM;
}
#endif /* CONFIG_STORAGE_MULTI */

#ifdef HAVE_HOTSWAP
bool hostfs_removable(IF_MD_NONVOID(int volume))
{
#ifdef HAVE_MULTIDRIVE
    if (volume > 0)
        return true;
    else
#endif
        return false; /* internal: always present */
}

bool hostfs_present(int volume)
{
#ifdef HAVE_MULTIDRIVE
    if (volume > 0)
#if defined(MULTIDRIVE_DEV)
        return os_file_exists(MULTIDRIVE_DEV);
#else
        return true; // FIXME?
#endif
    else
#endif
        return true; /* internal: always present */
}
#endif /* HAVE_HOTSWAP */

#ifdef HAVE_MULTIDRIVE
int volume_drive(int drive)
{
    return drive;
}
#endif /* HAVE_MULTIDRIVE */

#ifdef HAVE_HOTSWAP
bool volume_removable(IF_MV_NONVOID(int volume))
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_removable(volume);
}

bool volume_present(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_present(volume);
}
#endif /* HAVE_HOTSWAP */

int volume_partition(int volume)
{
    (void)volume;
    /* Hosted only implement a single parition per "drive" */
    return 0;
}
