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

/* A tuner the kernel already drives, reached through its V4L2 radio node.
 *
 * The other tuner drivers here own a chip: they put registers on a bus and
 * implement the tuning themselves. This one does not. On the Sony NW-A30 the
 * Si4708 has a vendor driver with no published source, and the only thing it
 * offers is /dev/radio0 - which turns out to be enough, because V4L2's radio
 * API covers everything Rockbox asks a tuner for.
 *
 * Audio does not come through here. The tuner is an analog input on the codec
 * (AIN2, via PGA2 and ADC2), so the sound is routed by the mixer rather than
 * read from this device; see audiohw_set_playback_src().
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "config.h"
#include "tuner.h"
#include "v4l2-radio.h"

#ifndef V4L2_RADIO_DEV
#define V4L2_RADIO_DEV "/dev/radio0"
#endif

static int radio_fd = -1;
static bool tuner_present = false;
/* V4L2 counts frequency in 1/16 MHz, or in 1/16 kHz when the tuner says
 * V4L2_TUNER_CAP_LOW. Getting this wrong is a factor of a thousand, so read it
 * from the tuner rather than assume. */
static bool freq_is_low = false;
static bool forced_mono = false;
static int cached_freq = 0;

static bool query_tuner(struct v4l2_tuner *tuner)
{
    memset(tuner, 0, sizeof(*tuner));
    tuner->index = 0;
    return radio_fd >= 0 && ioctl(radio_fd, VIDIOC_G_TUNER, tuner) == 0;
}

static int hz_to_v4l2(int hz)
{
    return freq_is_low ? (int)((long long)hz * 16 / 1000) : hz * 16 / 1000000;
}

static int v4l2_to_hz(int value)
{
    return freq_is_low ? (int)((long long)value * 1000 / 16) : value * 1000000 / 16;
}

static void close_radio(void)
{
    if(radio_fd >= 0)
        close(radio_fd);
    radio_fd = -1;
}

/* Returns false and leaves the device closed if this is not a radio we can
 * drive, so RADIO_PRESENT can answer honestly. */
static bool open_radio(void)
{
    if(radio_fd >= 0)
        return true;

    radio_fd = open(V4L2_RADIO_DEV, O_RDWR);
    if(radio_fd < 0)
    {
        printf("tuner: %s: %s\n", V4L2_RADIO_DEV, strerror(errno));
        return false;
    }

    struct v4l2_capability caps;
    memset(&caps, 0, sizeof(caps));
    if(ioctl(radio_fd, VIDIOC_QUERYCAP, &caps) != 0)
    {
        printf("tuner: %s is not a V4L2 device (%s)\n", V4L2_RADIO_DEV,
            strerror(errno));
        close_radio();
        return false;
    }
    printf("tuner: %s: %s (%s), caps 0x%08x\n", V4L2_RADIO_DEV, caps.card,
        caps.driver, caps.capabilities);

    struct v4l2_tuner tuner;
    if(!query_tuner(&tuner))
    {
        printf("tuner: no tuner behind %s (%s)\n", V4L2_RADIO_DEV,
            strerror(errno));
        close_radio();
        return false;
    }
    freq_is_low = (tuner.capability & V4L2_TUNER_CAP_LOW) != 0;
    printf("tuner: '%s' %d..%d (%s), caps 0x%08x\n", tuner.name,
        v4l2_to_hz(tuner.rangelow), v4l2_to_hz(tuner.rangehigh),
        freq_is_low ? "1/16 kHz" : "1/16 MHz", tuner.capability);
    fflush(stdout);
    return true;
}

static void set_audmode(bool mono)
{
    struct v4l2_tuner tuner;
    if(!query_tuner(&tuner))
        return;
    tuner.audmode = mono ? V4L2_TUNER_MODE_MONO : V4L2_TUNER_MODE_STEREO;
    ioctl(radio_fd, VIDIOC_S_TUNER, &tuner);
}

static bool set_frequency(int hz)
{
    struct v4l2_frequency freq;
    memset(&freq, 0, sizeof(freq));
    freq.tuner = 0;
    freq.type = V4L2_TUNER_RADIO;
    freq.frequency = hz_to_v4l2(hz);
    if(ioctl(radio_fd, VIDIOC_S_FREQUENCY, &freq) != 0)
    {
        printf("tuner: cannot tune to %d Hz: %s\n", hz, strerror(errno));
        return false;
    }
    cached_freq = hz;
    return true;
}

void v4l2_radio_init(void)
{
    /* Only to find out whether there is a tuner at all: the radio screen is
     * not reachable when RADIO_PRESENT is false, and holding the device open
     * from here would keep it powered for the whole session. */
    tuner_present = open_radio();
    close_radio();
}

int v4l2_radio_set(int setting, int value)
{
    switch(setting)
    {
        case RADIO_SLEEP:
            /* value != 0 means go to sleep */
            if(value)
                close_radio();
            else if(!open_radio())
                return 0;
            return 1;

        case RADIO_FREQUENCY:
            if(!open_radio())
                return 0;
            return set_frequency(value) ? 1 : 0;

        case RADIO_SCAN_FREQUENCY:
        {
            if(!open_radio())
                return 0;
            /* Tune and let the driver report whether anything is there. The
             * hardware seek this chip has scans on its own schedule, which does
             * not fit Rockbox's "try this one frequency" scan. */
            if(!set_frequency(value))
                return 0;
            struct v4l2_tuner tuner;
            if(!query_tuner(&tuner))
                return 0;
            return tuner.signal > 0 ? 1 : 0;
        }

        case RADIO_MUTE:
            /* The audio path is the codec's analog input, not this device, so
             * muting belongs to the mixer. Accept the call so the radio screen
             * does not treat it as a failure. */
            return 1;

        case RADIO_FORCE_MONO:
            if(!open_radio())
                return 0;
            forced_mono = value != 0;
            set_audmode(forced_mono);
            return 1;

        default:
            return 0;
    }
}

int v4l2_radio_get(int setting)
{
    struct v4l2_tuner tuner;

    switch(setting)
    {
        case RADIO_PRESENT:
            return tuner_present ? 1 : 0;

        case RADIO_TUNED:
            /* "tuned" for Rockbox means the last requested frequency carries a
             * station, which is what the signal strength says. */
            if(!query_tuner(&tuner))
                return 0;
            return tuner.signal > 0 ? 1 : 0;

        case RADIO_STEREO:
            if(forced_mono || !query_tuner(&tuner))
                return 0;
            return (tuner.rxsubchans & V4L2_TUNER_SUB_STEREO) ? 1 : 0;

        case RADIO_RSSI:
            if(!query_tuner(&tuner))
                return 0;
            /* V4L2 reports 0..65535; the radio screen wants the scale it gets
             * from RADIO_RSSI_MIN/MAX below. */
            return tuner.signal >> 8;

        case RADIO_RSSI_MIN:
            return 0;

        case RADIO_RSSI_MAX:
            return 255;

        default:
            return 0;
    }
}
