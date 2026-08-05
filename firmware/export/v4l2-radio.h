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
#ifndef _V4L2_RADIO_H_
#define _V4L2_RADIO_H_

#include <stdbool.h>

void v4l2_radio_init(void);
int v4l2_radio_set(int setting, int value);
int v4l2_radio_get(int setting);

#ifndef CONFIG_TUNER_MULTI
#define tuner_set v4l2_radio_set
#define tuner_get v4l2_radio_get
#endif

#endif /* _V4L2_RADIO_H_ */
