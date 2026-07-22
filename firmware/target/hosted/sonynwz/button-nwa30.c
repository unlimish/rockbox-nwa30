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

#include "system.h"
#include "button.h"
#include "button-target.h"

/* NW-A30 key layout. The player has volume up and down, rew, ff, play/pause, a
 * power button and a hold switch. The keycodes below were read off a real
 * NW-A35 by pressing each button in turn; they are the plain linux ones:
 *
 *   rew        105  KEY_LEFT
 *   ff         106  KEY_RIGHT
 *   play/pause  28  KEY_ENTER
 *   volume up  115  KEY_VOLUMEUP
 *   volume down 114  KEY_VOLUMEDOWN
 *   power       116  KEY_POWER
 *   hold         35  KEY_H, reported as a key rather than a switch and
 *                    handled by button-devinput.c, see button-target.h
 *   touchscreen 330  BTN_TOUCH, from the touchscreen's own device
 *
 * Note that they do NOT match the capability bitmap the keypad advertises in
 * /proc/bus/input/devices (HOME, END, POWER, MENU, BACK and some Sony-specific
 * codes): the keys the player actually sends come from a separate driver
 * (wm_key), so the bitmap is no guide to the layout. Trust this list, which was
 * measured, over anything derived from that one.
 *
 * The touchscreen ("himax-hx8526-icx") reports BTN_TOUCH on its own device;
 * button-devinput.c reads every device and calls this to translate keycodes. */

static int keycode_to_button(int keycode)
{
    switch(keycode)
    {
        case KEY_VOLUMEUP:
            return BUTTON_VOL_UP;
        case KEY_VOLUMEDOWN:
            return BUTTON_VOL_DOWN;
        case KEY_LEFT:      /* rew */
            return BUTTON_LEFT;
        case KEY_RIGHT:     /* ff */
            return BUTTON_RIGHT;
        case KEY_ENTER:     /* play/pause */
            return BUTTON_PLAY;
        case BTN_TOUCH:
            return BUTTON_TOUCH;
        /* The power button is the only one left, and this player has nothing
         * else to go back or up with (the touchscreen does the rest), so give
         * it BUTTON_BACK. That also makes it power the player off when held,
         * since POWEROFF_BUTTON is BUTTON_BACK on this keypad. */
        case KEY_POWER:
            return BUTTON_BACK;
        /* No button sends these, but the keypad claims to support them, so
         * make sure a stray event cannot trigger anything. */
        case KEY_HOME:
        case KEY_BACK:
        case KEY_MENU:
            return 0;
        default:
            /* logged by button_map() */
            return 0;
    }
}

/* Each keycode the player produces is reported once, so that pressing a button
 * this file does not handle still leaves its code in the log. */
static void log_keycode_once(int keycode, int button)
{
    static int seen[16];
    static unsigned nr_seen = 0;
    for(unsigned i = 0; i < nr_seen; i++)
        if(seen[i] == keycode)
            return;
    if(nr_seen < ARRAYLEN(seen))
        seen[nr_seen++] = keycode;
    printf("button: keycode %d (0x%x) -> %s (0x%x)\n", keycode, keycode,
        button ? "mapped" : "IGNORED", button);
}

int button_map(int keycode)
{
    int button = keycode_to_button(keycode);
    log_keycode_once(keycode, button);
    return button;
}

/* button_hold() comes from button-devinput.c, which tracks BUTTON_HOLD_KEYCODE.
 *
 * called by power-nwz.c after a suspend cycle: no event is generated for a
 * switch that was toggled while we were not listening, so re-read it */
void nwz_button_reload_after_suspend(void)
{
    button_reload_hold_status();
}

bool headphones_inserted(void)
{
    /* TODO: find out how headphone detection is exposed on the Hagoromo
     * platform (probably sysfs or the codec driver). Assume present for now
     * so that audio is never needlessly muted. */
    return true;
}
