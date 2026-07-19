/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (c) 2016 Amaury Pouly
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

#include "logf.h"
#include "system.h"
#include "kernel.h"
#include "string.h"
#include "stdio.h"
#include "audio.h"
#include "sound.h"
#include "audiohw.h"
#include "nwzlinux_codec.h"
#include "stdlib.h"
#include "panic.h"
#include <sys/ioctl.h>
#include "nwz_audio.h"
#include "pcm-alsa.h"
#include "alsa-controls.h"

/* This driver handle the Sony linux audio drivers: despite using many differents
 * codecs, it appears that they all share a common interface and common controls. */

#ifdef SONY_NWA30
/* Value we ask the CXD3778GF "master volume" control for. The OF drives it over
 * a 0-120 range; the driver clamps to its own maximum, so asking for the top of
 * that range is enough to take the hardware out of the way and leave the actual
 * attenuation to the digital volume. */
#define NWZ_MASTER_VOLUME_MAX   120
#endif

/* This is the alsa mixer interface exposed by Sony:
numid=3,iface=MIXER,name='Capture Src Switch'
  ; type=ENUMERATED,access=rw------,values=1,items=4
  ; Item #0 'None'
  ; Item #1 'Line'
  ; Item #2 'Fm'
  ; Item #3 'Mic'
  : values=0
numid=2,iface=MIXER,name='Playback Src Switch'
  ; type=ENUMERATED,access=rw------,values=1,items=7
  ; Item #0 'None'
  ; Item #1 'Music'
  ; Item #2 'Video'
  ; Item #3 'Tv'
  ; Item #4 'Fm'
  ; Item #5 'Line'
  ; Item #6 'Mic'
  : values=1
numid=1,iface=MIXER,name='Playback Volume'
  ; type=INTEGER,access=rw------,values=2,min=0,max=100,step=1
  : values=5,5
numid=7,iface=MIXER,name='CODEC Acoustic Switch'
  ; type=BOOLEAN,access=rw------,values=1
  : values=on
numid=8,iface=MIXER,name='CODEC Cue/Rev Switch'
  ; type=BOOLEAN,access=rw------,values=1
  : values=off
numid=9,iface=MIXER,name='CODEC Fade In Switch'
  ; type=BOOLEAN,access=rw------,values=1
  : values=off
numid=6,iface=MIXER,name='CODEC Mute Switch'
  ; type=BOOLEAN,access=rw------,values=1
  : values=off
numid=5,iface=MIXER,name='CODEC Power Switch'
  ; type=BOOLEAN,access=rw------,values=1
  : values=on
numid=10,iface=MIXER,name='CODEC Stanby Switch'
  ; type=BOOLEAN,access=rw------,values=1
  : values=off
numid=4,iface=MIXER,name='Output Switch'
  ; type=ENUMERATED,access=rw------,values=1,items=4
  ; Item #0 'Headphone'
  ; Item #1 'LineVariable'
  ; Item #2 'LineFixed'
  ; Item #3 'Speaker'
  : values=0
numid=6,iface=MIXER,name='Sampling Rate'
  ; type=ENUMERATED,access=rw------,values=1,items=6
  ; Item #0 '44100'
  ; Item #1 '48000'
  ; Item #2 '88200'
  ; Item #3 '96000'
  ; Item #4 '176400'
  ; Item #5 '192000'
  : values=0
*/

/* List of various codecs used by Sony */
enum nwz_codec_t
{
    NWZ_CS42L56,
    NWZ_R2A15602LG_D,
    NWZ_CS47L01_A,
    NWZ_CS47L01_D,
    NWZ_CXD3774GF_D,
    NWZ_CXD3778GF_D,
    NWZ_UNK_CODEC,
};

#define NWZ_LEVEL_MUTE  -1000
/* Description of the volume curve implemented by the kernel driver */
struct nwz_vol_curve_t
{
    int count; /* number of levels */
    int level[]; /* levels in tenth-dB, level[0] is always mute */
};

/* file descriptor of the icx_noican device */
static int fd_noican;
/* file descriptor of the hardware sound device */
static int fd_hw;
/* Codec */
static enum nwz_codec_t nwz_codec;
/* does the code support setting the sample rate? */
static bool has_sample_rate;

static enum nwz_codec_t find_codec(void)
{
    if(nwz_is_kernel_module_loaded("cs42L56_d"))
        return NWZ_CS42L56;
    if(nwz_is_kernel_module_loaded("r2A15602LG_d"))
        return NWZ_R2A15602LG_D;
    if(nwz_is_kernel_module_loaded("cs47L01_d"))
        return NWZ_CS47L01_D;
    if(nwz_is_kernel_module_loaded("cs47L01_a"))
        return NWZ_CS47L01_A;
    if(nwz_is_kernel_module_loaded("cxd3774gf_d"))
        return NWZ_CXD3774GF_D;
    if(nwz_is_kernel_module_loaded("cxd3778gf_d"))
        return NWZ_CXD3778GF_D;
    return NWZ_UNK_CODEC;
}

const char *nwz_get_codec_name(void)
{
    switch(nwz_codec)
    {
        case NWZ_CS42L56: return "cs42L56_d";
        case NWZ_R2A15602LG_D: return "r2A15602LG_d";
        case NWZ_CS47L01_D: return "cs47L01_d";
        case NWZ_CS47L01_A: return "cs47L01_a";
        case NWZ_CXD3774GF_D: return "cxd3774gf_d";
        case NWZ_CXD3778GF_D: return "cxd3778gf_d";
        default: return "Unknown";
    }
}

static struct nwz_vol_curve_t cxd3774gf_vol_curve =
{
    .count = 31,
    /* Most Sonys seem to follow the convention of 3dB/step then 2dB/step then 1dB/step */
    .level = {NWZ_LEVEL_MUTE,
        -550, -520, -490, -460, -430, -400, -370, -340, -310, -280, -250, /* 3dB/step */
        -230, -210, -190, -170, -150, -130, -110, -90, /* 2dB/step */
        -80, -70, -60, -50, -40, -30, -20, -10, 0, /* 1dB/step */
        15, 35, /* 1.5dB then 2dB */
    }
};

struct nwz_vol_curve_t *nwz_get_codec_vol_curve(void)
{
    switch(nwz_codec)
    {
        case NWZ_CS47L01_A:
        case NWZ_CS47L01_D:
            /* there are 32 levels but the last two are the same so in fact it
             * is the same curve as the cxd3774gf_d */
        case NWZ_CXD3774GF_D:
            return &cxd3774gf_vol_curve;
        default:
            /* return the safest curve (only 31 levels) */
            return &cxd3774gf_vol_curve;
    }
}

static void noican_init(void)
{
    fd_noican = open(NWZ_NC_DEV, O_RDWR);
    /* some targets don't have noise cancelling so silently fail */
}

static void noican_close(void)
{
    if(fd_noican >= 0)
        close(fd_noican);
}

/* Set NC switch */
static void noican_set_switch(int sw)
{
    if(ioctl(fd_noican, NWZ_NC_SET_SWITCH, &sw) < 0)
        panicf("ioctl(NWZ_NC_SET_SWITCH) failed");
}

/* Get NC switch */
static int noican_get_switch(void)
{
    int val;
    if(ioctl(fd_noican, NWZ_NC_GET_SWITCH, &val) < 0)
        panicf("ioctl(NWZ_NC_GET_SWITCH) failed");
    return val;
}

/* Get HP status */
static int noican_get_hp_status(void)
{
    int val;
    if(ioctl(fd_noican, NWZ_NC_GET_HP_STATUS, &val) < 0)
        panicf("ioctl(NWZ_NC_GET_HP_STATUS) failed");
    return val;
}

/* Set HP type */
static void noican_set_hp_type(int type)
{
    if(ioctl(fd_noican, NWZ_NC_SET_HP_TYPE, &type) < 0)
        panicf("ioctl(NWZ_NC_SET_HP_TYPE) failed");
}

/* Get HP type */
static int noican_get_hp_type(void)
{
    int val;
    if(ioctl(fd_noican, NWZ_NC_GET_HP_TYPE, &val) < 0)
        panicf("ioctl(NWZ_NC_GET_HP_TYPE) failed");
    return val;
}


/* Set gain */
static void noican_set_gain(int gain)
{
    if(ioctl(fd_noican, NWZ_NC_SET_GAIN, &gain) < 0)
        panicf("ioctl(NWZ_NC_SET_GAIN) failed");
}

/* Get gain */
static int noican_get_gain(void)
{
    int val;
    if(ioctl(fd_noican, NWZ_NC_GET_GAIN, &val) < 0)
        panicf("ioctl(NWZ_NC_GET_GAIN) failed");
    return val;
}

/* Set filter */
static void noican_set_filter(int filter)
{
    if(ioctl(fd_noican, NWZ_NC_SET_FILTER, &filter) < 0)
        panicf("ioctl(NWZ_NC_SET_FILTER) failed");
}

/* Get filter */
static int noican_get_filter(void)
{
    int val;
    if(ioctl(fd_noican, NWZ_NC_GET_FILTER, &val) < 0)
        panicf("ioctl(NWZ_NC_GET_FILTER) failed");
    return val;
}

static void hw_open(void)
{
    fd_hw = open("/dev/snd/hwC0D0", O_RDWR);
    if(fd_hw < 0)
    {
        /* the fd is not actually used for anything at the moment, so only
         * panic on platforms where we know it must exist */
#ifdef SONY_NWA30
        printf("Cannot open '/dev/snd/hwC0D0', ignored\n");
#else
        panicf("Cannot open '/dev/snd/hwC0D0'");
#endif
    }
}

static void hw_close(void)
{
    close(fd_hw);
}

/* Acoustic and Cue/Rev control how the volume curve, but it is not clear
 * what the intention of these modes are and the OF does not seem to use
 * them by default */
bool audiohw_acoustic_enabled(void)
{
#ifdef SONY_NWA30
    return false; /* no such control on the Hagoromo platform */
#else
    return alsa_controls_get_bool("CODEC Acoustic Switch");
#endif
}

void audiohw_enable_acoustic(bool en)
{
#ifdef SONY_NWA30
    (void)en;
#else
    alsa_controls_set_bool("CODEC Acoustic Switch", en);
#endif
}

bool audiohw_cuerev_enabled(void)
{
#ifdef SONY_NWA30
    return false;
#else
    return alsa_controls_get_bool("CODEC Cue/Rev Switch");
#endif
}

void audiohw_enable_cuerev(bool en)
{
#ifdef SONY_NWA30
    (void)en;
#else
    alsa_controls_set_bool("CODEC Cue/Rev Switch", en);
#endif
}

void audiohw_set_playback_src(enum nwz_src_t src)
{
#ifdef SONY_NWA30
    /* The CXD3778GF has no source multiplexer: PCM playback always goes
     * through the digital path, while the tuner is an analog input that is
     * mixed in separately. Enabling it is what the OF does to listen to the
     * radio (control names confirmed in the device kernel). */
    switch(src)
    {
        case NWZ_RADIO:
            alsa_controls_set_bool("analog input device", true);
            alsa_controls_set_bool("analog playback mute", false);
            break;
        case NWZ_MIC: /* not wired up on this platform */
        case NWZ_PLAYBACK:
        default:
            alsa_controls_set_bool("analog input device", false);
            alsa_controls_set_bool("analog playback mute", true);
            break;
    }
#else
    switch(src)
    {
        case NWZ_RADIO: alsa_controls_set_enum("Playback Src Switch", "Fm"); break;
        case NWZ_MIC: alsa_controls_set_enum("Playback Src Switch", "Mic"); break;
        case NWZ_PLAYBACK:
        default: alsa_controls_set_enum("Playback Src Switch", "Music"); break;
    }
#endif
}

void audiohw_preinit(void)
{
    alsa_controls_init("default");
#ifdef SONY_NWA30
    /* The Hagoromo platform (CXD3778GF) has its own set of controls; the names
     * used here were all confirmed to exist in the device's own kernel.
     * Note that "playback mute" is the digital (PCM) path, which is the one we
     * care about: "analog playback mute" belongs to the analog input used for
     * the tuner and is handled in audiohw_set_playback_src(). */
    alsa_controls_set_bool("playback mute", false);
    /* make sure the tuner passthrough is not mixed into playback */
    audiohw_set_playback_src(NWZ_PLAYBACK);
    /* Keep the hardware "master volume" at its maximum and do the actual
     * attenuation digitally, see audiohw_set_volume(). The control range is
     * not documented, so ask the driver for it instead of assuming 0-120. */
    if(alsa_has_control("master volume"))
    {
        int vol_cnt;
        alsa_controls_get_info("master volume", &vol_cnt);
        if(vol_cnt > 0 && vol_cnt <= 2)
        {
            long vols[2] = {NWZ_MASTER_VOLUME_MAX, NWZ_MASTER_VOLUME_MAX};
            alsa_controls_set_ints("master volume", vol_cnt, vols);
        }
    }
    /* the CXD3778GF has no "Sampling Rate" control (checked in the kernel),
     * this simply leaves has_sample_rate false */
    has_sample_rate = alsa_has_control("Sampling Rate");
#else
    /* turn on codec */
    alsa_controls_set_bool("CODEC Power Switch", true);
    /* mute */
    alsa_controls_set_bool("CODEC Mute Switch", true);
    /* Acoustic and Cue/Rev control how the volume curve, but it is not clear
     * what the intention of these modes are and the OF does not seem to use
     * them by default */
    audiohw_enable_acoustic(false);
    audiohw_enable_cuerev(false);
    /* select playback source */
    audiohw_set_playback_src(NWZ_PLAYBACK);
    /* use headphone output */
    alsa_controls_set_enum("Output Switch", "Headphone");
    /* unmute */
    alsa_controls_set_bool("CODEC Mute Switch", false);
    /* sample rate */
    has_sample_rate = alsa_has_control("Sampling Rate");
#endif

    /* init noican */
    noican_init();
    if(fd_noican >= 0)
    {
        /* dump configuration, for debug purposes */
        printf("nc hp status: %d\n", noican_get_hp_status());
        printf("nc type: %d\n", noican_get_hp_type());
        printf("nc switch: %d\n", noican_get_switch());
        printf("nc gain: %d\n", noican_get_gain());
        printf("nc filter: %d\n", noican_get_filter());
        /* make sure we start in a clean state */
        noican_set_switch(NWZ_NC_SWITCH_OFF);
        noican_set_hp_type(NC_HP_TYPE_DEFAULT);
        noican_set_filter(NWZ_NC_FILTER_INDEX_0);
        noican_set_gain(NWZ_NC_GAIN_CENTER);
    }

    /* init hw */
    hw_open();
    nwz_codec = find_codec();
    printf("Codec: %s\n", nwz_get_codec_name());
}

void audiohw_postinit(void)
{
}

/* volume must be driver unit */
static void nwz_set_driver_vol(int vol)
{
    long vols[2];
    /* the driver expects percent, convert from centibel in range 0...x */
    vols[0] = vols[1] = vol;
    /* on some recent players like A10, Sony decided to merge left/right volume
     * into one, thus we need to make sure we write the correct number of values */
    int vol_cnt;
    alsa_controls_get_info("Playback Volume", &vol_cnt);
    alsa_controls_set_ints("Playback Volume", vol_cnt, vols);
}

/* volume is in tenth-dB */
void audiohw_set_volume(int vol_l, int vol_r)
{
#ifdef SONY_NWA30
    /* The Hagoromo volume curve is defined by a model-specific table loaded
     * into the codec (see /proc/icx_audio_cxd3778gf_data/ovt), so we cannot
     * reproduce it here. Instead keep the hardware "master volume" at maximum
     * and do everything with the digital volume, which is always safe. */
    int min_pcm = -430;
    int max_pcm = 0;
    if(vol_l < min_pcm)
        vol_l = min_pcm;
    if(vol_l > max_pcm)
        vol_l = max_pcm;
    if(vol_r < min_pcm)
        vol_r = min_pcm;
    if(vol_r > max_pcm)
        vol_r = max_pcm;
    pcm_set_mixer_volume(vol_l, vol_r);
#else
    /* FIXME at the moment we don't support balance and just average left and right.
     * But this could be implemented using pcm alsa digital volume */

    /* the Sony drivers expect vol_l = vol_r */
    int vol = (vol_l + vol_r) / 2;
    printf("request volume %d dB\n", vol / 10);
    struct nwz_vol_curve_t *curve = nwz_get_codec_vol_curve();
    /* min/max for pcm volume */
    int min_pcm = -430;
    int max_pcm = 0;
    /* On some codecs (like cs47L01), Sony clear overdrives the DAC which produces
     * massive clipping at any level (since they fix the DAC volume at around +6dB
     * and then adjust HP volume in negative at the top of range !!). The only
     * solution around this problem is to use the digital volume first so that
     * very quickly the digital volume compensate for the DAC overdrive and we
     * avoid clipping. */
    int sony_clip_level = -80; /* any volume above this will cause massive clipping the DAC */

    /* to avoid the clipping problem, virtually decrease requested volume by the
     * clipping threshold, so that we will compensate in digital later by
     * at least this amount if possibly */
    vol -= sony_clip_level;

    int drv_vol = curve->count - 1;
    /* pick driver level just above request volume */
    while(drv_vol > 0 && curve->level[drv_vol - 1] >= vol)
        drv_vol--;
    /* now remove the artifical volume change */
    vol += sony_clip_level;
    /* now adjust digital volume */
    vol -= curve->level[drv_vol];
    if(vol < min_pcm)
    {
        vol = min_pcm; /* digital cannot do <43dB */
        drv_vol = 0; /* mute */
    }
    else if(vol > max_pcm)
        vol = max_pcm; /* digital cannot do >0dB */
    printf(" set driver volume %d (%d dB)\n", drv_vol, curve->level[drv_vol] / 10);
    nwz_set_driver_vol(drv_vol);
    printf(" set digital volume %d dB\n", vol / 10);
    pcm_set_mixer_volume(vol, vol);
#endif /* SONY_NWA30 */
}

void audiohw_close(void)
{
    hw_close();
    alsa_controls_close();
    noican_close();
}

void audiohw_set_frequency(int fsel)
{
    if(has_sample_rate)
    {
        /* it's slightly annoying that Sony put the value in an enum with strings... */
        char freq_str[16];
        sprintf(freq_str, "%d", fsel);
        alsa_controls_set_enum("Sampling Rate", freq_str);
    }
}
