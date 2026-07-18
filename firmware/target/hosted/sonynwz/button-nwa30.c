/***************************************************************************
 *             __________               __   ___
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
#include <linux/input.h>

#include "button.h"
#include "button-target.h"

/* NW-A30 key layout (Hagoromo platform):
 * Unlike the older icx-based players, the keys are reported as standard
 * linux input events spread over several devices:
 *   /dev/input/event0: power + hold switch
 *   /dev/input/event1: touchscreen
 *   /dev/input/event4: playback keys
 * The mapping below follows what the stock kernel reports.
 * The hold switch (KEY_H) is handled directly by button-devinput.c
 * (BUTTON_HOLD_KEYCODE) and never reaches this map. */
int button_map(int keycode)
{
    switch(keycode)
    {
        case KEY_ENTER: /* play/pause */
            return BUTTON_PLAY;
        case KEY_LEFT: /* previous/rewind */
            return BUTTON_REW;
        case KEY_RIGHT: /* next/fast-forward */
            return BUTTON_FF;
        case KEY_VOLUMEDOWN:
            return BUTTON_VOL_DOWN;
        case KEY_VOLUMEUP:
            return BUTTON_VOL_UP;
        case KEY_POWER:
            return BUTTON_BACK;
        case BTN_TOUCH:
            return BUTTON_TOUCH;
        default:
            return 0;
    }
}

/* called by power-nwz.c after a suspend cycle: events generated while
 * suspended are lost, so re-read the hold switch state */
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
