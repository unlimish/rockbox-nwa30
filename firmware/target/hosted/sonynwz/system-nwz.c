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

#include "logf.h"

static const char **kern_mod_list;
bool os_file_exists(const char *ospath);

void power_off(void)
{
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

/* Refuse to run twice.
 *
 * The application we replace gets killed every half minute or so and started
 * again, and Rockbox now outlives that by running in its own session - so the
 * copy that replaces us can find one already running and start a second. Two of
 * them fight over the framebuffer and the input devices, which looks like
 * everything at once misbehaving.
 *
 * Take a lock rather than look for another process by name: the lock is held by
 * the kernel for exactly as long as the process lives, so it cannot go stale,
 * and it does not care how the other copy was started. */
#if defined(SONY_NWA30) && !defined(BOOTLOADER)
static void claim_single_instance(void)
{
    int fd = open("/tmp/rockbox.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if(fd < 0)
        return; /* cannot tell - carry on rather than refuse to start */
    if(flock(fd, LOCK_EX | LOCK_NB) < 0)
    {
        printf("another rockbox is already running, exiting\n");
        fflush(stdout);
        exit(0);
    }
    /* deliberately leaked: the lock must be held for our whole lifetime */
}
#endif

#ifdef SONY_NWA30
/* Why the player last started. The system reboots out from under us about half
 * a minute after we take over, and the kernel command line records the reason,
 * which says whether that is the watchdog or something else. */
static void print_boot_reason(void)
{
    FILE *f = fopen("/proc/cmdline", "re");
    if(f == NULL)
        return;
    char line[1024];
    if(fgets(line, sizeof(line), f) != NULL)
    {
        const char *p = strstr(line, "bootreason=");
        if(p != NULL)
        {
            p += sizeof("bootreason=") - 1;
            printf("Boot reason: %.*s\n", (int)strcspn(p, " \n"), p);
        }
    }
    fclose(f);
}

/* Why the player restarted.
 *
 * It reboots about half a minute after we take over - the whole machine, not
 * just us: /proc/uptime starts from scratch each time. The stock init arms a 30
 * second watchdog (exec /bin/wdt_ctrl 30, which talks to /proc/wdk), but that
 * one is kicked by kernel threads rather than by the application, so it should
 * not fire just because we are the ones running.
 *
 * Rather than keep guessing, print what the machine itself records: the state
 * of the watchdog, and the tail of the previous boot's kernel log, which is
 * where a watchdog timeout, a panic or a deliberate restart would each say so
 * in their own words. */
static void print_file(const char *path, const char *what, long tail_bytes)
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

static void print_restart_evidence(void)
{
    print_file("/proc/wdk", "watchdog state", 0);
    /* the previous boot's kernel log, under the two names it goes by */
    print_file("/proc/last_kmsg", "previous kernel log", 2048);
    print_file("/sys/fs/pstore/console-ramoops", "previous console", 2048);
}

/* Nothing to pet: the watchdog on this platform is kicked by the kernel. */
void nwz_watchdog_pet(void)
{
}
#endif

/* How long the machine has been up. We get restarted every half minute or so;
 * this says whether only our process is being restarted (uptime keeps rising
 * across restarts) or the whole player is rebooting (it goes back to zero),
 * which decides whether outliving the app we replaced can help at all. */
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

/* Walk /proc: for every process, call fn(pid, cmdline). We run in place of the
 * stock application while the rest of the stock userspace keeps running, so
 * this is how we find both what to log and what to stop. */
static void for_each_process(void (*fn)(int pid, const char *cmdline))
{
    DIR *proc = opendir("/proc");
    if(proc == NULL)
        return;
    struct dirent *entry;
    while((entry = readdir(proc)))
    {
        /* only numeric entries are processes */
        if(entry->d_name[0] < '1' || entry->d_name[0] > '9')
            continue;
        char path[sizeof("/proc//cmdline") + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE *f = fopen(path, "re");
        if(f == NULL)
            continue;
        char cmd[128];
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if(n == 0)
            continue; /* kernel thread, no cmdline */
        /* cmdline is NUL-separated argv, turn the separators into spaces */
        for(size_t i = 0; i < n; i++)
            if(cmd[i] == 0)
                cmd[i] = ' ';
        cmd[n] = 0;
        fn(atoi(entry->d_name), cmd);
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

/* Stop the application manager from restarting the player.
 *
 * The stock application reports to it over the Sony IPC framework once it is up
 * ("FireChangeLifeCycle"); we do not, so it times out waiting for the home
 * application to reach the foreground and has the machine restarted - which is
 * what the previous boot's kernel log shows, an orderly reboot run by a process
 * called "reboot" about half a minute in.
 *
 * Freeze it rather than kill it: killed, init would start it again and the same
 * countdown would begin; stopped, it stays in the process table and simply
 * never runs the timeout. This is a blunt instrument standing in for speaking
 * its protocol, and it does mean the stock application management is out of
 * action for as long as Rockbox is running.
 */
#if !defined(BOOTLOADER)
/* Find who runs the hang checker.
 *
 * The framework in libpstcore.so watches applications' main threads and, when
 * it decides one is stuck, restarts the machine:
 *
 *     Hang detected! pid=%s
 *     Application(pid=%s) main thread is freezed, reboot the system...
 *
 * The previous boot's kernel log names the thread that asks for the restart -
 * "fr_job" - but that library is loaded by every hagodaemon process, so the
 * name alone does not say which one. Look through each process's threads for
 * it, which does.
 */
static void find_thread_owner(int pid, const char *cmdline, const char *thread,
                              bool freeze)
{
    char dir[32];
    snprintf(dir, sizeof(dir), "/proc/%d/task", pid);
    DIR *task = opendir(dir);
    if(task == NULL)
        return;
    struct dirent *tid;
    while((tid = readdir(task)))
    {
        if(tid->d_name[0] < '1' || tid->d_name[0] > '9')
            continue;
        char path[sizeof(dir) + 1 + NAME_MAX + sizeof("/comm")];
        snprintf(path, sizeof(path), "%s/%s/comm", dir, tid->d_name);
        FILE *f = fopen(path, "re");
        if(f == NULL)
            continue;
        char comm[32] = {0};
        if(fgets(comm, sizeof(comm), f) != NULL)
        {
            comm[strcspn(comm, "\n")] = 0;
            if(strcmp(comm, thread) == 0)
            {
                printf("thread '%s' belongs to pid %d: %s\n", thread, pid, cmdline);
                if(freeze)
                {
                    printf("freezing it\n");
                    kill(pid, SIGSTOP);
                }
            }
        }
        fclose(f);
    }
    closedir(task);
}

/* Services we must leave running: without these the player stops appearing as
 * a USB drive, which is how it is loaded with music and how we get the log
 * back. Everything else in the framework can sit still while Rockbox runs. */
static bool service_is_needed(const char *cmdline)
{
    static const char *needed[] =
    {
        "UsbMgrServiceFw",           /* USB itself */
        "UsbHostConnectionService",  /* and the connection state it acts on */
        "StorageMgrServiceFw",       /* the storage behind the drive */
    };
    for(unsigned i = 0; i < sizeof(needed) / sizeof(needed[0]); i++)
        if(strstr(cmdline, needed[i]) != NULL)
            return true;
    return false;
}

static void handle_hang_checker(int pid, const char *cmdline)
{
    /* "fr_job" turned out to be a worker thread the framework core gives every
     * one of its daemons, so it identifies the framework rather than the one
     * service that restarts us. Freezing all of them does stop the restarts,
     * but it also took USB with it. Spare the services that USB needs until we
     * know which daemon actually runs the hang check. */
    if(service_is_needed(cmdline))
    {
        printf("leaving alone: pid %d %s\n", pid, cmdline);
        return;
    }
    find_thread_owner(pid, cmdline, "fr_job", true);
}
#endif

#if !defined(BOOTLOADER)
static void freeze_app_manager(int pid, const char *cmdline)
{
    if(strstr(cmdline, "appmgrservice") != NULL)
    {
        printf("freezing app manager (pid %d)\n", pid);
        kill(pid, SIGSTOP);
    }
}
#endif

static void kill_boot_animation(int pid, const char *cmdline)
{
    /* The stock boot splash keeps drawing over our screen because nothing told
     * it boot was done. It is just the animation, so end it the blunt way; the
     * stock app stops it too. */
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
    /* Our stdout is a file on the player, so it is block buffered by default:
     * anything printed before an abrupt death (a signal we cannot handle, say)
     * is lost with the buffer, which leaves the log showing only the unbuffered
     * stderr output and makes it look as if we never got that far. Log lines as
     * they are written instead - this log is how the port gets debugged. */
    setvbuf(stdout, NULL, _IOLBF, 0);
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
    /* On this player something in the stock userspace sends us SIGTERM a little
     * while after we start, whether or not we are being used - the earlier logs
     * showed "terminated by the system" followed by a restart. That kills the
     * UI and, because the system restarts the app we replaced, loops. Ignore it
     * so Rockbox stays up. If it turns out the system then escalates to SIGKILL
     * we cannot catch that, but the next log will show whether this was enough.
     * (This does mean we do not hand the player back on a SIGTERM meant to grab
     * it for USB mode; revisit once we know where the signal comes from.) */
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

#endif
    print_proc_list();
    for_each_process(kill_boot_animation);
#if !defined(BOOTLOADER)
    for_each_process(freeze_app_manager);
    for_each_process(handle_hang_checker);
#endif
    /* some init not done on hosted targets */
    adc_init();
    power_init();
}


void system_reboot(void)
{
    power_off();
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
