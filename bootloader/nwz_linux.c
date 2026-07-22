/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2016 by Amaury Pouly
 *
 * Based on Rockbox iriver bootloader by Linus Nielsen Feltzing
 * and the ipodlinux bootloader by Daniel Palffy and Bernard Leach
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

#include "system.h"
#include "lcd.h"
#include "backlight.h"
#include "button-target.h"
#include "button.h"
#include "../kernel/kernel-internal.h"
#include "core_alloc.h"
#include "filesystem-app.h"
#include "nvp-nwz.h"
#include "power-nwz.h"
#include "lcd.h"
#include "font.h"
#include "power.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include "version.h"

/* all images must have the following size */
#define ICON_WIDTH  130
#define ICON_HEIGHT 130

/* images */
#include "bitmaps/rockboxicon.h"
#include "bitmaps/toolsicon.h"

/* don't issue an error when parsing the file for dependencies */
#if defined(BMPWIDTH_rockboxicon) && (BMPWIDTH_rockboxicon != ICON_WIDTH || \
    BMPHEIGHT_rockboxicon != ICON_HEIGHT)
#error rockboxicon has the wrong resolution
#endif
#if defined(BMPWIDTH_toolsicon) && (BMPWIDTH_toolsicon != ICON_WIDTH || \
    BMPHEIGHT_toolsicon != ICON_HEIGHT)
#error toolsicon has the wrong resolution
#endif

/* the A860 does not have left/right/up/down but it has rew/ff so pretend we
 * always have rew/ff */
#ifndef BUTTON_REW
#define BUTTON_REW  BUTTON_LEFT
#endif
#ifndef BUTTON_FF
#define BUTTON_FF   BUTTON_RIGHT
#endif

/* Buffer for the Sony image, filled from NVP. The image stored in NVP is
 * RGB565 but this is drawn as FORMAT_NATIVE, so it has to be kept in whatever
 * the LCD uses (the NW-A30 is a 32-bit target, where assuming RGB565 here used
 * to overrun this buffer by a factor of two). */
fb_data sonyicon[ICON_WIDTH * ICON_HEIGHT];

/* convert one RGB565 pixel, as stored in NVP, to the native format
 * (FB_RGBPACK copes with fb_data being a struct on 32-bit targets) */
static inline fb_data rgb565_to_native(unsigned short v)
{
    unsigned r = (v >> 11) & 0x1f;
    unsigned g = (v >> 5) & 0x3f;
    unsigned b = v & 0x1f;
    /* replicate the high bits so that white stays white */
    return FB_RGBPACK((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}
const struct bitmap bm_sonyicon =
{
    .width = ICON_WIDTH,
    .height = ICON_HEIGHT,
    .format = FORMAT_NATIVE,
    .data = (unsigned char*)sonyicon
};

/* return icon y position (x is always centered) */
int get_icon_y(void)
{
    /* adjust so that this contains the Sony logo and produces a nice logo
     * when used with rockbox */
    if(LCD_HEIGHT == 320)
        return 70;
    else if(LCD_HEIGHT == 400)
        return 100;
    else
        /* Both values above put the centre of the icon at about 41% of the
         * screen height, which leaves room for the description and the timeout
         * line underneath. Keep those proportions on taller screens such as
         * the NW-A30's 800px one, where the old guess drifted too low. */
        return LCD_HEIGHT * 41 / 100 - ICON_HEIGHT / 2;
}

#define ROCKBOX_BIN        "/contents/.rockbox/rockbox.sony"
#define ROCKBOX_LIB_DIR    "/contents/.rockbox/lib"
#define ROCKBOX_CODECS_DIR "/contents/.rockbox/codecs"
#define ROCKBOX_ARGV0      "rockbox.sony"
/* Staging area on a filesystem we are allowed to execute from, see
 * boot_rockbox(). /tmp is a 32MB tmpfs on this platform. */
#define STAGE_DIR       "/tmp/rockbox"
#define STAGE_CODECS_DIR STAGE_DIR "/codecs"

static bool write_all(int fd, const char *buf, size_t len)
{
    while(len > 0)
    {
        ssize_t written = write(fd, buf, len);
        if(written <= 0)
            return false;
        buf += written;
        len -= written;
    }
    return true;
}

/* copy a file, giving the copy the requested mode. Returns true on success. */
static bool copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if(in < 0)
        return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if(out < 0)
    {
        close(in);
        return false;
    }
    bool ok = true;
    char buf[16384];
    ssize_t len;
    while(ok && (len = read(in, buf, sizeof(buf))) > 0)
        ok = write_all(out, buf, len);
    if(len < 0)
        ok = false;
    /* the mode given to open() is masked by the umask, so be explicit */
    if(ok && fchmod(out, mode) < 0)
        ok = false;
    if(close(out) < 0)
        ok = false;
    close(in);
    if(!ok)
        unlink(dst);
    return ok;
}

/* Copy every file in src_dir into dst_dir, which is created if missing.
 * Non-recursive; used to stage both the .so libraries and the codecs. */
static void copy_dir_flat(const char *src_dir, const char *dst_dir)
{
    mkdir(dst_dir, 0755);
    DIR *dir = opendir(src_dir);
    if(dir == NULL)
        return;
    struct dirent *entry;
    while((entry = readdir(dir)))
    {
        /* big enough for the prefix plus any name readdir can return */
        char src[sizeof(ROCKBOX_LIB_DIR) + 1 + NAME_MAX];
        char dst[sizeof(STAGE_CODECS_DIR) + 1 + NAME_MAX];
        if(entry->d_name[0] == '.')
            continue;
        snprintf(src, sizeof(src), "%s/%s", src_dir, entry->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, entry->d_name);
        if(!copy_file(src, dst, 0755))
            printf("cannot stage %s: %s\n", src, strerror(errno));
    }
    closedir(dir);
}

/* Something in the stock userspace kills the application we replaced about half
 * a minute after it starts, so Rockbox is given a session of its own and
 * outlives us. The system then starts us again in its place, and we must not
 * launch a second Rockbox on top of the first. */
static bool rockbox_is_running(void)
{
    bool found = false;
    DIR *proc = opendir("/proc");
    if(proc == NULL)
        return false;
    struct dirent *entry;
    while(!found && (entry = readdir(proc)))
    {
        if(entry->d_name[0] < '1' || entry->d_name[0] > '9')
            continue;
        char path[sizeof("/proc//cmdline") + NAME_MAX];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE *f = fopen(path, "re");
        if(f == NULL)
            continue;
        char cmd[64];
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if(n == 0)
            continue;
        cmd[n] = 0;
        /* cmdline starts with argv[0] and is NUL terminated */
        if(strcmp(cmd, ROCKBOX_ARGV0) == 0)
            found = true;
    }
    closedir(proc);
    return found;
}

/* defined below, next to main() where the log is first opened */
int open_log(void);

/* Everything Rockbox needs, copied where it can be executed from.
 *
 * The user partition holding .rockbox is a FAT filesystem this platform does
 * not let us execute from: exec'ing rockbox.sony off it fails with EACCES, and
 * mapping our bundled libasound out of a noexec mount fails too. Codecs are
 * worse still - they are dlopen()'d at runtime, which is rejected both straight
 * from /contents and for anything the *running* rockbox.sony writes into tmpfs
 * itself. Only files that were already there before exec can be loaded, which
 * is why the codecs have to be staged here rather than by Rockbox. See
 * nwa30_stage_for_exec() in lc-unix.c for the matching lookup. */
static void stage_rockbox_libraries(void)
{
    copy_dir_flat(ROCKBOX_LIB_DIR, STAGE_DIR);
    copy_dir_flat(ROCKBOX_CODECS_DIR, STAGE_CODECS_DIR);
    setenv("LD_LIBRARY_PATH", STAGE_DIR, 1);
}

/* ENOENT from exec does not mean the binary is missing - we just wrote it - but
 * that its ELF interpreter is, which is what a soft-float build looks like. */
static void report_exec_failure(const char *path)
{
    int err = errno; /* printf() is free to clobber errno */
    printf("cannot fork: %s\n", strerror(err));
    if(err != ENOENT)
        return;
    struct stat st;
    printf("  staged size: %ld\n",
        stat(path, &st) == 0 ? (long)st.st_size : -1L);
    printf("  /lib/ld-linux-armhf.so.3 (hard float): %s\n",
        access("/lib/ld-linux-armhf.so.3", F_OK) == 0 ? "present" : "MISSING");
    printf("  /lib/ld-linux.so.3 (soft float): %s\n",
        access("/lib/ld-linux.so.3", F_OK) == 0 ? "present" : "MISSING");
}

/* Wait for Rockbox without holding the log open. The log lives on /contents and
 * one descriptor there is enough to keep the partition busy, which would stop
 * Rockbox handing it to a USB host. We must not keep a dup() to restore from,
 * since that descriptor holds the file just as firmly; reopen instead. Nothing
 * is printed in between - we are about to block. */
static int wait_for_rockbox(pid_t pid)
{
    int devnull = open("/dev/null", O_WRONLY);
    if(devnull >= 0)
    {
        fflush(stdout);
        fflush(stderr);
        dup2(devnull, fileno(stdout));
        dup2(devnull, fileno(stderr));
        close(devnull);
    }
    int status;
    waitpid(pid, &status, 0);
    int log = open_log();
    if(log >= 0)
    {
        dup2(log, fileno(stdout));
        dup2(log, fileno(stderr));
        close(log);
    }
    return status;
}

/* Returns true if Rockbox ran and has since exited, false if it could not be
 * started at all. Falls back to running it in place, which is what the icx
 * players do and costs nothing to try. */
static bool boot_rockbox(void)
{
    char path[64];

    mkdir(STAGE_DIR, 0755);
    snprintf(path, sizeof(path), "%s/%s", STAGE_DIR, ROCKBOX_ARGV0);
    if(!copy_file(ROCKBOX_BIN, path, 0755))
    {
        printf("cannot stage %s in %s: %s\n", ROCKBOX_BIN, STAGE_DIR,
            strerror(errno));
        fflush(stdout);
        execl(ROCKBOX_BIN, ROCKBOX_ARGV0, NULL);
        return false;
    }

    stage_rockbox_libraries();
    printf("booting %s\n", path);
    fflush(stdout);

    /* Rockbox gets a session of its own rather than running in our place: the
     * application we stand in for is killed about half a minute after it starts
     * - SIGTERM, then SIGKILL if that is ignored - and would take Rockbox with
     * it. In its own session it is no longer the process being killed. */
    pid_t pid = fork();
    if(pid == 0)
    {
        setsid();
        execl(path, ROCKBOX_ARGV0, NULL);
        printf("cannot run %s: %s\n", path, strerror(errno));
        fflush(stdout);
        _exit(1);
    }
    if(pid > 0)
    {
        /* Waiting means that leaving Rockbox returns here. If we are killed
         * first, Rockbox keeps running and whatever replaces us stands aside. */
        printf("rockbox exited (status %d)\n", wait_for_rockbox(pid));
        return true;
    }

    report_exec_failure(path);
    fflush(stdout);
    execl(ROCKBOX_BIN, ROCKBOX_ARGV0, NULL);
    return false;
}

/* Sony logo extraction */
bool extract_sony_logo(void)
{
    /* load the entire image from the nvp */
    int bti_size = nwz_nvp_read(NWZ_NVP_BTI, NULL);
    if(bti_size < 0)
        return false;
    /* compute the offset in the image of the logo itself */
    int x_off = (LCD_WIDTH - ICON_WIDTH) / 2; /* logo is centered horizontally */
    int y_off = get_icon_y();
    /* The image is a full screen RGB565 picture. Make sure it really is big
     * enough for the region we are about to copy out of it, rather than
     * reading past the end of the buffer if this device stores something else
     * (or something smaller) in that node. */
    size_t needed = (size_t)LCD_WIDTH * (y_off + ICON_HEIGHT) * sizeof(unsigned short);
    if((size_t)bti_size < needed)
    {
        printf("nvp boot image is too small (%d bytes, need %zu)\n", bti_size, needed);
        return false;
    }
    unsigned short *bti = malloc(bti_size);
    if(bti == NULL)
        return false;
    if(nwz_nvp_read(NWZ_NVP_BTI, bti) != bti_size)
    {
        free(bti);
        return false;
    }
    /* extract part of the image, converting to the native pixel format */
    for(int y = 0; y < ICON_HEIGHT; y++)
    {
        const unsigned short *src = bti + LCD_WIDTH * (y + y_off) + x_off;
        fb_data *dst = sonyicon + ICON_WIDTH * y;
        for(int x = 0; x < ICON_WIDTH; x++)
            dst[x] = rgb565_to_native(src[x]);
    }
    free(bti);
    return true;
}

/* Important Note: this bootloader is carefully written so that in case of
 * error, the OF is run. This seems like the safest option since the OF is
 * always there and might do magic things. */

enum boot_mode
{
    BOOT_ROCKBOX,
    BOOT_TOOLS,
    BOOT_OF,
    BOOT_COUNT,
    BOOT_USB, /* special */
    BOOT_STOP, /* power down/suspend */
};

static void display_text_center(int y, const char *text)
{
    int width;
    lcd_getstringsize(text, &width, NULL);
    lcd_putsxy(LCD_WIDTH / 2 - width / 2, y, text);
}

static void display_text_centerf(int y, const char *format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);

    vsnprintf(buf, sizeof(buf), format, ap);
    display_text_center(y, buf);
}

/* get timeout before taking action if the user doesn't touch the device */
int get_inactivity_tmo(void)
{
    if(button_hold())
        return 5 * HZ; /* Inactivity timeout when on hold */
    else
        return 10 * HZ; /* Inactivity timeout when not on hold */
}

/* return action on idle timeout */
enum boot_mode inactivity_action(enum boot_mode cur_selection)
{
    if(button_hold())
        return BOOT_STOP; /* power down/suspend */
    else
        return cur_selection; /* return last choice */
}

/* we store the boot mode in a file in /tmp so we can reload it between 'boots'
 * (since the mostly suspends instead of powering down) */
enum boot_mode load_boot_mode(enum boot_mode mode)
{
    int fd = open("/tmp/rb_bl_mode.txt", O_RDONLY);
    if(fd >= 0)
    {
        read(fd, &mode, sizeof(mode));
        close(fd);
    }
    return mode;
}

void save_boot_mode(enum boot_mode mode)
{
    int fd = open("/tmp/rb_bl_mode.txt", O_RDWR | O_CREAT | O_TRUNC);
    if(fd >= 0)
    {
        write(fd, &mode, sizeof(mode));
        close(fd);
    }
}

enum boot_mode get_boot_mode(void)
{
    /* load previous mode, or start with rockbox if none */
    enum boot_mode init_mode = load_boot_mode(BOOT_ROCKBOX);
    /* wait for user action */
    enum boot_mode mode = init_mode;
    int last_activity = current_tick;
    bool hold_status = button_hold();
    while(true)
    {
#ifndef SONY_NWA30
        /* on usb detect, return to usb
         * FIXME this is a hack, we need proper usb detection */
        if(power_input_status() & POWER_INPUT_USB_CHARGER)
        {
            /* save last choice */
            save_boot_mode(mode);
            return BOOT_USB;
        }
#else
        /* Not on this player. BOOT_USB here means "run the original firmware
         * because it is the only thing that can do USB", but the framework is
         * still running while we sit in this menu and already presents the
         * user partition to the host - so plugging the cable in is how the
         * player gets loaded with music, and jumping to the OF the moment it
         * is connected would take that away. */
#endif
        /* inactivity detection */
        int timeout = last_activity + get_inactivity_tmo();
        if(TIME_AFTER(current_tick, timeout))
        {
            /* save last choice */
            save_boot_mode(mode);
            return inactivity_action(mode);
        }
        /* redraw */
        lcd_clear_display();
        /* display top text */
        if(button_hold())
        {
            lcd_set_foreground(LCD_RGBPACK(255, 0, 0));
            display_text_center(0, "ON HOLD!");
        }
        else
        {
            lcd_set_foreground(LCD_RGBPACK(255, 201, 0));
            display_text_center(0, "SELECT PLAYER");
        }
        lcd_set_foreground(LCD_RGBPACK(255, 201, 0));
        /* display icon */
        const struct bitmap *icon = (mode == BOOT_OF) ? &bm_sonyicon :
            (mode == BOOT_ROCKBOX) ? &bm_rockboxicon : &bm_toolsicon;
        lcd_bmp(icon, (LCD_WIDTH - ICON_WIDTH) / 2, get_icon_y());
        /* display bottom description */
        const char *desc = (mode == BOOT_OF) ? "SONY" :
            (mode == BOOT_ROCKBOX) ? "ROCKBOX" : "TOOLS";
        display_text_center(get_icon_y() + ICON_HEIGHT + 30, desc);
        /* display arrows */
        int arrow_width, arrow_height;
        lcd_getstringsize("<", &arrow_width, &arrow_height);
        int arrow_y = get_icon_y() + ICON_HEIGHT / 2 - arrow_height / 2;
        lcd_putsxy(arrow_width / 2, arrow_y, "<");
        lcd_putsxy(LCD_WIDTH - 3 * arrow_width / 2, arrow_y, ">");

        lcd_set_foreground(LCD_RGBPACK(0, 255, 0));
        display_text_centerf(LCD_HEIGHT - arrow_height * 3 / 2, "timeout in %d sec",
            (timeout - current_tick + HZ - 1) / HZ);

        lcd_update();

        /* wait for a key  */
        int btn = button_get_w_tmo(HZ / 10);
        /* record action, changing HOLD counts as action */
        if(btn & BUTTON_MAIN || hold_status != button_hold())
            last_activity = current_tick;
        hold_status = button_hold();
        /* ignore release, allow repeat */
        if(btn & BUTTON_REL)
            continue;
        if(btn & BUTTON_REPEAT)
            btn &= ~BUTTON_REPEAT;
        /* play -> stop loop and return mode */
        if(btn == BUTTON_PLAY)
            break;
        /* left/right/up/down: change mode. The volume keys do the same: they
         * are the one pair every one of these players is guaranteed to have,
         * and on a target whose other keycodes are not identified yet they are
         * the difference between a usable menu and being stuck on whatever
         * happens to be selected. */
        if(btn == BUTTON_LEFT || btn == BUTTON_DOWN || btn == BUTTON_REW ||
           btn == BUTTON_VOL_DOWN)
            mode = (mode + BOOT_COUNT - 1) % BOOT_COUNT;
        if(btn == BUTTON_RIGHT || btn == BUTTON_UP || btn == BUTTON_FF ||
           btn == BUTTON_VOL_UP)
            mode = (mode + 1) % BOOT_COUNT;
    }

    /* save mode */
    save_boot_mode(mode);
    return mode;
}

void error_screen(const char *msg)
{
    lcd_clear_display();
    lcd_putsf(0, 0, msg);
    lcd_update();
}

void create_sony_logo(void)
{
    /* nothing better to show than a placeholder, but at least make it a
     * native-format one instead of a hardcoded RGB565 value */
    for(int y = 0; y < ICON_HEIGHT; y++)
        for(int x = 0; x < ICON_WIDTH; x++)
            sonyicon[y * ICON_WIDTH + x] = FB_RGBPACK(255, 0, 255);
}

int choice_screen(const char *title, bool center, int nr_choices, const char *choices[])
{
    int choice = 0;
    int max_len = 0;
    int h;
    lcd_getstringsize("x", NULL, &h);
    for(int i = 0; i < nr_choices; i++)
    {
        int len = strlen(choices[i]);
        if(len > max_len)
            max_len = len;
    }
    char *buf = malloc(max_len + 10);
    int top_y = 2 * h;
    int nr_lines = (LCD_HEIGHT - top_y) / h;
    while(true)
    {
        /* make sure choice is visible */
        int offset = choice - nr_lines / 2;
        if(offset < 0)
            offset = 0;
        lcd_clear_display();
        /* display top text */
        lcd_set_foreground(LCD_RGBPACK(255, 201, 0));
        display_text_center(0, title);
        int line = 0;
        for(int i = 0; i < nr_choices && line < nr_lines; i++)
        {
            if(i < offset)
                continue;
            if(i == choice)
                lcd_set_foreground(LCD_RGBPACK(255, 0, 0));
            else
                lcd_set_foreground(LCD_RGBPACK(255, 201, 0));
            sprintf(buf, "%s", choices[i]);
            if(center)
                display_text_center(top_y + h * line, buf);
            else
                lcd_putsxy(0, top_y + h * line, buf);
            line++;
        }

        lcd_update();

        /* wait for a key  */
        int btn = button_get_w_tmo(HZ / 10);
        /* ignore release, allow repeat */
        if(btn & BUTTON_REL)
            continue;
        if(btn & BUTTON_REPEAT)
            btn &= ~BUTTON_REPEAT;
        /* play -> stop loop and return mode */
        if(btn == BUTTON_PLAY || btn == BUTTON_BACK)
        {
            free(buf);
            return btn == BUTTON_PLAY ? choice : -1;
        }
        /* left/right/up/down: change mode (volume keys too, see get_boot_mode) */
        if(btn == BUTTON_LEFT || btn == BUTTON_UP || btn == BUTTON_REW ||
           btn == BUTTON_VOL_DOWN)
            choice = (choice + nr_choices - 1) % nr_choices;
        if(btn == BUTTON_RIGHT || btn == BUTTON_DOWN || btn == BUTTON_FF ||
           btn == BUTTON_VOL_UP)
            choice = (choice + 1) % nr_choices;
    }
}

void run_file(const char *name)
{
    char *dirname = "/contents/";
    char *buf = malloc(strlen(dirname) + strlen(name) + 1);
    sprintf(buf, "%s%s", dirname, name);

    lcd_clear_display();
    lcd_set_foreground(LCD_RGBPACK(255, 201, 0));
    lcd_putsf(0, 0, "Running %s", name);
    lcd_update();

    pid_t pid = fork();
    if(pid == 0)
    {
        execlp("sh", "sh", buf, NULL);
        _exit(42);
    }
    int status;
    waitpid(pid, &status, 0);
    if(WIFEXITED(status))
    {
        lcd_set_foreground(LCD_RGBPACK(255, 201, 0));
        lcd_putsf(0, 1, "program returned %d", WEXITSTATUS(status));
    }
    else
    {
        lcd_set_foreground(LCD_RGBPACK(255, 0, 0));
        lcd_putsf(0, 1, "an error occured: %x", status);
    }
    lcd_set_foreground(LCD_RGBPACK(255, 0, 0));
    lcd_putsf(0, 3, "Press any key or wait");
    lcd_update();
    /* wait a small time */
    sleep(HZ);
    /* ignore event */
    while(button_get(false) != 0) {}
    /* wait for any key or timeout */
    button_get_w_tmo(4 * HZ);
}

void run_script_menu(void)
{
    const char **entries = NULL;
    int nr_entries = 0;
    DIR *dir = opendir("/contents/");
    struct dirent *ent;
    while((ent = readdir(dir)))
    {
        if(ent->d_type != DT_REG)
            continue;
        entries = realloc(entries, (nr_entries + 1) * sizeof(const char *));
        entries[nr_entries++] = strdup(ent->d_name);
    }
    closedir(dir);
    int idx = choice_screen("RUN SCRIPT", false, nr_entries, entries);
    if(idx >= 0)
        run_file(entries[idx]);
    for(int i = 0; i < nr_entries; i++)
        free((char *)entries[i]);
    free(entries);
}

void tools_screen(void)
{
    const char *choices[] = {"Service menu", "Run script", "Restart", "Shutdown"};
    int choice = choice_screen("TOOLS MENU", true, 4, choices);
    if(choice == 0)
    {
        /* run service menu */
        fflush(stdout);
        execl("/usr/local/bin/mptapp", "mptapp", NULL);
        error_screen("Cannot boot service menu");
        sleep(5 * HZ);
    }
    else if(choice == 1)
        run_script_menu();
    else if(choice == 2)
        nwz_power_restart();
    else if(choice == 3)
        nwz_power_shutdown();
}

#define LOG_FILE      "/contents/rockbox.log"
#define LOG_FILE_OLD  LOG_FILE ".1"
#define LOG_MAX_SIZE  1000000

/* Open the log, rotating it first if it has grown too big. The mode matters:
 * O_CREAT without one uses whatever is on the stack, and this is the only log
 * the port has - an unreadable one has cost whole debugging sessions. */
int open_log(void)
{
    int fd = open(LOG_FILE, O_RDWR | O_CREAT | O_APPEND, 0666);
    struct stat stat;
    if(fstat(fd, &stat) != 0 || stat.st_size < LOG_MAX_SIZE)
        return fd;
    close(fd);
    rename(LOG_FILE, LOG_FILE_OLD);
    /* truncate in case the rename did not happen */
    return open(LOG_FILE, O_RDWR | O_CREAT | O_APPEND | O_TRUNC, 0666);
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    /* redirect stdout and stderr to have error messages logged somewhere on the
     * user partition */
    int fd = open_log();
    if(fd >= 0)
    {
        dup2(fd, fileno(stdout));
        dup2(fd, fileno(stderr));
        close(fd);
    }
    /* print version */
    printf("Rockbox boot loader\n");
    printf("Version: %s\n", rbversion);
    printf("%s\n", MODEL_NAME);

    /* The system restarts the application we replaced after it kills it, so we
     * may well be starting on top of a Rockbox that is still running. Say so
     * and get out of the way: touching the screen or the buttons from here
     * would fight with it. */
    if(rockbox_is_running())
    {
        printf("rockbox is already running, standing by\n");
        fflush(stdout);
        while(true)
            sleep(60 * HZ);
    }

    system_init();
    core_allocator_init();
    kernel_init();
    paths_init();
    lcd_init();
    font_init();
    button_init();
    backlight_init();
    backlight_set_brightness(DEFAULT_BRIGHTNESS_SETTING);
    /* Try to load the extra font we install on the device: sysfont is far too
     * small to be readable, especially on the high-dpi screens. The icx players
     * get it installed into the root filesystem, but that is read-only (and has
     * no /usr/local at all) on the Hagoromo platform, so there we ship it in
     * .rockbox on the user partition instead. */
    static const char *font_paths[] =
    {
        "/usr/local/share/rockbox/bootloader.fnt",
        "/contents/.rockbox/bootloader.fnt",
    };
    for(unsigned i = 0; i < sizeof(font_paths) / sizeof(font_paths[0]); i++)
    {
        int font_id = font_load(font_paths[i]);
        if(font_id >= 0)
        {
            printf("loaded font %s\n", font_paths[i]);
            lcd_setfont(font_id);
            break;
        }
    }
    /* extract logo */
    if(!extract_sony_logo())
        create_sony_logo();
    /* run all tools menu */
    while(true)
    {
        enum boot_mode mode = get_boot_mode();
        if(mode == BOOT_USB || mode == BOOT_OF)
        {
            fflush(stdout);
            fflush(stderr);
            /* Stop logging, but hand the original firmware a sane set of file
             * descriptors: leaving 1 and 2 closed means the first file it opens
             * becomes its stdout, and everything it prints lands in that file. */
            int devnull = open("/dev/null", O_WRONLY);
            if(devnull >= 0)
            {
                dup2(devnull, fileno(stdout));
                dup2(devnull, fileno(stderr));
                if(devnull > fileno(stderr))
                    close(devnull);
            }
            /* We ignore SIGTERM to survive being told to quit; the original
             * firmware must not inherit that, since answering it is how the
             * system takes the player back (for USB, say). Ignored signals do
             * carry across exec, so put it back first. */
            signal(SIGTERM, SIG_DFL);
            /* for now the only way we have to trigger USB mode it to run the OF */
            /* boot OF */
            execvp(NWZ_OF_APP, argv);
            error_screen("Cannot boot OF");
            sleep(5 * HZ);
        }
        else if(mode == BOOT_TOOLS)
        {
            tools_screen();
        }
        else if(mode == BOOT_ROCKBOX)
        {
            /* Rockbox expects /.rockbox to contain themes, rocks, etc, but we
            * cannot easily create this symlink because the root filesystem is
            * mounted read-only. Although we could remount it read-write temporarily,
            * this is neededlessly complicated and we defer this job to the dualboot
            * install script */
            fflush(stdout);
            if(!boot_rockbox())
            {
                printf("execvp failed: %s\n", strerror(errno));
                /* fallback to OF in case of failure */
                error_screen("Cannot boot Rockbox");
                sleep(5 * HZ);
            }
            /* otherwise Rockbox ran and has quit, so come back to the menu */
        }
        else
        {
            printf("suspend\n");
            nwz_power_suspend();
        }
    }
    /* if we reach this point, everything failed, so return an error so that
     * sysmgrd knows something is wrong */
    return 1;
}
