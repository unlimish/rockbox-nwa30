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
#include <stdio.h>
#include <stdbool.h>
#include "powermgmt.h"
#include "power.h"
#include "power-nwz.h"

unsigned short battery_level_disksafe = 3470;

/* the OF shuts down at this voltage */
unsigned short battery_level_shutoff = 3450;

/* voltages (millivolt) of 0%, 10%, ... 100% when charging disabled */
unsigned short percent_to_volt_discharge[11] =
{
    3450, 3698, 3746, 3781, 3792, 3827, 3882, 3934, 3994, 4060, 4180
};

/* voltages (millivolt) of 0%, 10%, ... 100% when charging enabled */
unsigned short percent_to_volt_charge[11] =
{
      3450, 3670, 3721, 3751, 3782, 3821, 3876, 3941, 4034, 4125, 4200
};

unsigned int power_input_status(void)
{
    unsigned pwr = 0;
    int sts = nwz_power_get_status();
    /* The driver returns -1 if it cannot be reached, and -1 has every bit set,
     * so testing it directly would report both chargers as present. That is
     * the worst possible answer: the bootloader takes "USB connected" as a cue
     * to hand over to the OF, so a missing power device would mean Rockbox
     * never gets to start. Report no external power instead. */
    if(sts < 0)
        return 0;
    if(sts & NWZ_POWER_STATUS_VBUS_DET)
        pwr |= POWER_INPUT_USB_CHARGER;
    if(sts & NWZ_POWER_STATUS_AC_DET)
        pwr |= POWER_INPUT_MAIN_CHARGER;
    return pwr;
}

int _battery_voltage(void)
{
    /* the raw voltage is unstable on some devices, so use the average provided
     * by the driver */
    int mv = nwz_power_get_battery_voltage();
    /* The driver reports -1 when it cannot be reached at all, e.g. when the
     * power device does not exist on this player. Reporting that as-is would
     * put us below battery_level_shutoff and power the device off moments
     * after booting, which is very hard to tell apart from a crash. Claim a
     * nominal mid-charge voltage instead: the battery reading is wrong either
     * way, but at least the player stays usable. */
    if(mv <= 0)
    {
        static bool warned = false;
        if(!warned)
        {
            printf("battery: no reading from the driver, assuming nominal\n");
            warned = true;
        }
        return percent_to_volt_discharge[5];
    }
    return mv;
}

bool charging_state(void)
{
    int sts = nwz_power_get_status();
    if(sts < 0)
        return false; /* see power_input_status() */
    return (sts & NWZ_POWER_STATUS_CHARGE_STATUS) ==
        NWZ_POWER_STATUS_CHARGE_STATUS_CHARGING;
}
