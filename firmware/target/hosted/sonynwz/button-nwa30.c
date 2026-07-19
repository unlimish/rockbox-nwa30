/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026
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
#include <stdio.h>
#include <linux/input.h>

#include "button.h"
#include "button-target.h"

/* NW-A30 key layout (Hagoromo platform, confirmed by recon on a real NW-A35):
 *   /dev/input/event0: "mtk-kpd" hardware keypad (transport keys, volume,
 *                      power) - reports the keycodes handled below
 *   /dev/input/event1: "himax-hx8526-icx" capacitive touchscreen (BTN_TOUCH)
 * button-devinput.c reads every device and calls this to translate keycodes.
 *
 * The keypad exposes: HOME(102), END(107), VOLUMEDOWN(114), VOLUMEUP(115),
 * POWER(116), MENU(139), BACK(158) plus three Sony-custom codes 211/212/231
 * for the physical transport buttons whose exact assignment still needs to be
 * confirmed on the device - so we map them tentatively and log every keycode
 * we do NOT recognise (goes to /contents/rockbox.log) to make that easy. */

/* Sony-custom transport keycodes (not in linux/input.h) - tentative */
#define NWA30_KEY_A 211
#define NWA30_KEY_B 212
#define NWA30_KEY_C 231

int button_map(int keycode)
{
    switch(keycode)
    {
        case KEY_VOLUMEUP:
            return BUTTON_VOL_UP;
        case KEY_VOLUMEDOWN:
            return BUTTON_VOL_DOWN;
        case KEY_HOME:
        case KEY_BACK:
            return BUTTON_BACK;
        case KEY_MENU:
            return BUTTON_PLAY;
        /* tentative transport mapping, refine once confirmed on device */
        case NWA30_KEY_A:
            return BUTTON_LEFT;  /* guess: previous */
        case NWA30_KEY_B:
            return BUTTON_RIGHT; /* guess: next */
        case NWA30_KEY_C:
            return BUTTON_PLAY;  /* guess: play/pause */
        case BTN_TOUCH:
            return BUTTON_TOUCH;
        case KEY_POWER:
            /* deliberately ignored: mapping it to a navigation bit could
             * trigger unwanted actions, and software poweroff already works
             * by holding POWEROFF_BUTTON (BACK) */
            return 0;
        default:
            printf("button: unmapped keycode %d (0x%x)\n", keycode, keycode);
            return 0;
    }
}

/* The hold switch is not identified on this player yet (see button-target.h),
 * so report the player as never held rather than guessing at a keycode. */
bool button_hold(void)
{
    return false;
}

/* called by power-nwz.c after a suspend cycle: events generated while
 * suspended are lost and would have to be re-read here. There is nothing to
 * reload while the hold switch is unknown. */
void nwz_button_reload_after_suspend(void)
{
}

bool headphones_inserted(void)
{
    /* TODO: find out how headphone detection is exposed on the Hagoromo
     * platform (probably sysfs or the codec driver). Assume present for now
     * so that audio is never needlessly muted. */
    return true;
}
