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
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __NWZLINUX_CODEC_H__
#define __NWZLINUX_CODEC_H__

#ifdef SONY_NWA30
/* the S-Master amp's high/low gain range, offered as "DAC power mode" */
#define AUDIOHW_CAPS (POWER_MODE_CAP)
#else
#define AUDIOHW_CAPS 0
#endif

/* Ranges from -100dB to 4dB */
AUDIOHW_SETTING(VOLUME,       "dB", 0,  1, -100,  4, -10)

#ifdef SONY_NWA30
/* The S-Master amplifier has a second gain range, which is what the player
 * needs to reach a normal listening level with anything but the most
 * sensitive headphones. Expose it rather than fixing it: high gain is not
 * free - it costs battery and headroom - and which one suits depends on what
 * is plugged in. Rockbox's existing "DAC power mode" setting is exactly this
 * shape (a high/low choice, saved with the other sound settings), so use it
 * instead of inventing a control of our own. */
AUDIOHW_SETTING(POWER_MODE,     "", 0,  1,    0,  1,   0)
#endif

enum nwz_src_t
{
    NWZ_PLAYBACK,
    NWZ_RADIO,
    NWZ_MIC,
};

/* enable/disable Sony's "acoustic" mode */
bool audiohw_acoustic_enabled(void);
void audiohw_enable_acoustic(bool en);
/* enable/disable Sony's "cuerev" mode */
bool audiohw_cuerev_enabled(void);
void audiohw_enable_cuerev(bool en);
/* select playback source */
void audiohw_set_playback_src(enum nwz_src_t src);

#endif /* __NWZLINUX_CODEC_H__ */
