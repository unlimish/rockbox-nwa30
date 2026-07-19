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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include "lcd.h"
#include "lcd-target.h"
#include "backlight-target.h"

static int fb_fd = 0;
fb_data *nwz_framebuffer = 0; /* global variable, see lcd-target.h */
/* saved variable screen info, used to pan/flip on plain fbdev (mtkfb) */
static struct fb_var_screeninfo nwz_vinfo;
enum
{
    FB_MP200, /* MP200 fb driver */
    FB_EMXX, /* EMXX fb driver */
    FB_OTHER, /* unknown, assume plain fbdev (eg MediaTek on Hagoromo) */
}nwz_fb_type;

void identify_fb(const char *id)
{
    if(strcmp(id, "MP200 FB") == 0)
        nwz_fb_type = FB_MP200;
    else if(strcmp(id, "EMXX FB") == 0)
        nwz_fb_type = FB_EMXX;
    else
        nwz_fb_type = FB_OTHER;
    printf("lcd: fb id = '%s' -> type = %d\n", id, nwz_fb_type);
}

/* On Hagoromo (NW-A30 and later), the framebuffer is a plain fbdev device
 * without the Sony ioctls, and the backlight is handled through sysfs.
 * The driver name varies, so just pick the first entry with a brightness
 * attribute. */
static char sysfs_bl_brightness[128];
static char sysfs_bl_max[128];
static int sysfs_bl_max_value = 255;

/* try to use a specific led/backlight sysfs directory, return true on success */
static bool try_sysfs_backlight(const char *dir)
{
    snprintf(sysfs_bl_brightness, sizeof(sysfs_bl_brightness),
        "%s/brightness", dir);
    snprintf(sysfs_bl_max, sizeof(sysfs_bl_max),
        "%s/max_brightness", dir);
    /* the brightness node must be writable for us to use it */
    if(access(sysfs_bl_brightness, W_OK) != 0)
    {
        sysfs_bl_brightness[0] = 0;
        return false;
    }
    /* max_brightness is optional (led class always has it, but be safe) */
    sysfs_bl_max_value = 255;
    FILE *f = fopen(sysfs_bl_max, "re");
    if(f)
    {
        if(fscanf(f, "%d", &sysfs_bl_max_value) != 1)
            sysfs_bl_max_value = 255;
        fclose(f);
    }
    return true;
}

static void find_sysfs_backlight(void)
{
    sysfs_bl_brightness[0] = 0;
    /* on the Hagoromo platform (NW-A30, mt65xx_leds driver) the LCD backlight
     * is an led class device at this fixed path */
    if(try_sysfs_backlight("/sys/class/leds/lcd-backlight"))
        goto done;
    /* otherwise scan the generic backlight class */
    DIR *dir = opendir("/sys/class/backlight");
    if(dir != NULL)
    {
        struct dirent *entry;
        while((entry = readdir(dir)))
        {
            /* big enough for the prefix plus any name readdir can return */
            char path[sizeof("/sys/class/backlight/") + NAME_MAX];
            if(entry->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "/sys/class/backlight/%s", entry->d_name);
            if(try_sysfs_backlight(path))
                break;
        }
        closedir(dir);
    }
done:
    printf("lcd: sysfs backlight = '%s' (max %d)\n",
        sysfs_bl_brightness[0] ? sysfs_bl_brightness : "<none>", sysfs_bl_max_value);
}

static void sysfs_backlight_brightness(int brightness)
{
    if(sysfs_bl_brightness[0] == 0)
        return;
    FILE *f = fopen(sysfs_bl_brightness, "we");
    if(f == NULL)
        return;
    /* scale Rockbox 0-5 range to the driver range */
    fprintf(f, "%d", brightness * sysfs_bl_max_value / MAX_BRIGHTNESS_SETTING);
    fclose(f);
}

/* select which page (0 or 1) to display, disable DSP, transparency and rotation */
static int nwz_fb_set_page(int page)
{
    /* set page mode to no transparency and no rotation */
    struct nwz_fb_image_info mode_info;
    mode_info.tc_enable = 0;
    mode_info.t_color = 0;
    mode_info.alpha = 0;
    mode_info.rot = 0;
    mode_info.page = page;
    mode_info.update = NWZ_FB_ONLY_2D_MODE;
    if(ioctl(fb_fd, NWZ_FB_UPDATE, &mode_info) < 0)
        return -1;
    return 0;
}

/* make sure framebuffer is in standard state so rendering works */
static int nwz_fb_set_standard_mode(void)
{
    /* plain fbdev (Hagoromo): nothing to set up */
    if(nwz_fb_type == FB_OTHER)
        return 0;
    /* disable timer (apparently useless with LCD) */
    struct nwz_fb_update_timer update_timer;
    update_timer.timerflag = NWZ_FB_TIMER_OFF;
    update_timer.timeout = NWZ_FB_DEFAULT_TIMEOUT;
    /* we don't check the result of the next ioctl() because it will fail in
     * newer version of the driver, where the timer disapperared. */
    ioctl(fb_fd, NWZ_FB_UPDATE_TIMER, &update_timer);
    return nwz_fb_set_page(0);
}

void backlight_hw_brightness(int brightness)
{
    if(nwz_fb_type == FB_OTHER)
    {
        sysfs_backlight_brightness(brightness);
        return;
    }
    struct nwz_fb_brightness bl;
    bl.level = brightness; /* brightness level: 0-5 */
    bl.step = 25; /* number of hardware steps to do when changing: 1-100 (smooth transition) */
    bl.period = NWZ_FB_BL_MIN_PERIOD; /* period in ms between steps when changing: >=10 */

    ioctl(fb_fd, nwz_fb_type == FB_MP200 ? NWZ_FB_SET_BRIGHTNESS_MP200 : NWZ_FB_SET_BRIGHTNESS_EMXX, &bl);
}

bool backlight_hw_init(void)
{
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    return true;
}

void backlight_hw_on(void)
{
#ifdef HAVE_LCD_ENABLE
    lcd_enable(true); /* power on lcd + visible display */
#endif
    /* don't do anything special, the core will set the brightness */
}

void backlight_hw_off(void)
{
    /* there is no real on/off but we can set to 0 brightness */
    backlight_hw_brightness(0);
#ifdef HAVE_LCD_ENABLE
    lcd_enable(false); /* power off visible display */
#endif
}

void lcd_shutdown(void)
{
    munmap(nwz_framebuffer, FRAMEBUFFER_SIZE);
    close(fb_fd);
}

/* We cannot do anything useful without a framebuffer, but simply exiting is a
 * bad idea: on the Hagoromo platform we run in place of the stock application,
 * which the system restarts when it dies, so exiting turns into a boot loop
 * with the device stuck and no way in. Hand over to the original firmware
 * instead: the player boots normally and the reason is left in the log. */
static void lcd_init_failed(const char *what)
{
    printf("lcd: %s, booting the OF instead\n", what);
    fflush(stdout);
    execlp(NWZ_OF_APP, "of", NULL);
    /* if even that fails there is nothing left to try */
    printf("lcd: cannot run %s: %s\n", NWZ_OF_APP, strerror(errno));
    fflush(stdout);
    exit(0);
}

void lcd_init_device(void)
{
    /* old icx-based players use /dev/fb/0, Hagoromo exposes the framebuffer as
     * the Android-style /dev/graphics/fb0 (confirmed by recon); also try the
     * plain /dev/fb0 for good measure */
    fb_fd = open("/dev/fb/0", O_RDWR);
    if(fb_fd < 0)
        fb_fd = open("/dev/graphics/fb0", O_RDWR);
    if(fb_fd < 0)
        fb_fd = open("/dev/fb0", O_RDWR);
    if(fb_fd < 0)
    {
        perror("Cannot open framebuffer");
        lcd_init_failed("cannot open framebuffer");
    }

    /* get fixed and variable information */
    struct fb_fix_screeninfo finfo;
    if(ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        perror("Cannot read framebuffer fixed information");
        lcd_init_failed("cannot read framebuffer fixed information");
    }
    identify_fb(finfo.id);
    struct fb_var_screeninfo vinfo;
    if(ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        perror("Cannot read framebuffer variable information");
        lcd_init_failed("cannot read framebuffer variable information");
    }
    /* check resolution and framebuffer size */
    if(vinfo.xres != LCD_WIDTH || vinfo.yres != LCD_HEIGHT || vinfo.bits_per_pixel != LCD_DEPTH)
    {
        printf("Unexpected framebuffer resolution: %dx%dx%d (expected %dx%dx%d)\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
            LCD_WIDTH, LCD_HEIGHT, LCD_DEPTH);
        lcd_init_failed("unexpected framebuffer resolution");
    }
    /* Note: we use a framebuffer size of width*height*bbp. We cannot trust the
     * values returned by the driver for line_length */

    /* map framebuffer */
    nwz_framebuffer = mmap(0, FRAMEBUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if((void *)nwz_framebuffer == (void *)-1)
    {
        perror("Cannot map framebuffer");
        lcd_init_failed("cannot map framebuffer");
    }
    /* make sure rendering state is correct */
    nwz_fb_set_standard_mode();
    /* on plain fbdev devices, find the sysfs backlight interface */
    if(nwz_fb_type == FB_OTHER)
    {
        nwz_vinfo = vinfo;
        find_sysfs_backlight();
    }
}

static void redraw(void)
{
    if(nwz_fb_type == FB_OTHER)
    {
        /* The data is already in the visible framebuffer, but mtkfb does not
         * necessarily refresh the panel on plain writes, so pan to the
         * (unchanged) origin to ask it to push the frame to the display.
         * If the screen ever stays blank while the rest of the log looks
         * healthy, this is the thing to question: the driver may want a real
         * change of yoffset, i.e. drawing into alternating frames of the
         * virtual framebuffer instead of always into the first one. */
        static int pan_logged = 0;
        nwz_vinfo.xoffset = 0;
        nwz_vinfo.yoffset = 0;
        int ret = ioctl(fb_fd, FBIOPAN_DISPLAY, &nwz_vinfo);
        if(!pan_logged)
        {
            pan_logged = 1;
            printf("lcd: FBIOPAN_DISPLAY %s\n",
                ret < 0 ? "not supported by this driver" : "ok");
        }
        return;
    }
    nwz_fb_set_page(0);
}

extern void lcd_copy_buffer_rect(fb_data *dst, const fb_data *src,
                                 int width, int height);

void lcd_update(void)
{
    /* Copy the Rockbox framebuffer to the second framebuffer */
    lcd_copy_buffer_rect(LCD_FRAMEBUF_ADDR(0, 0), FBADDR(0,0),
                         LCD_WIDTH*LCD_HEIGHT, 1);
    redraw();
}

void lcd_update_rect(int x, int y, int width, int height)
{
    fb_data *dst = LCD_FRAMEBUF_ADDR(x, y);
    fb_data * src = FBADDR(x,y);

    /* Copy part of the Rockbox framebuffer to the second framebuffer */
    if (width < LCD_WIDTH)
    {
        /* Not full width - do line-by-line */
        lcd_copy_buffer_rect(dst, src, width, height);
    }
    else
    {
        /* Full width - copy as one line */
        lcd_copy_buffer_rect(dst, src, LCD_WIDTH*height, 1);
    }
    redraw();
}
