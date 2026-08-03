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
#include <dirent.h>
#include "fixedpoint.h"
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

#ifdef SONY_NWA30
#define PCM_DEVICE_OVERRIDE_FILE  "/.rockbox/pcm_device.txt"
#define MIXER_OVERRIDE_FILE       "/.rockbox/mixer.txt"
#define CODEC_PROC_DIR            "/proc/icx_audio_cxd3778gf_data"

/* alsa_controls_set_*() panics on a control that is missing or of another type,
 * and audiohw_preinit() runs during boot with no UI to report from. The control
 * names come from the device's own kernel but their types were never checked
 * there, so one wrong name would be a boot-blocking panic. Skipping leaves at
 * worst a muted player, which can be recovered from. */
static bool nwa30_control_is(const char *name, snd_ctl_elem_type_t want,
                             const char *what)
{
    snd_ctl_elem_type_t type = alsa_controls_get_type(name);
    if(type == want)
        return true;
    if(type == SND_CTL_ELEM_TYPE_NONE)
        printf("audio: mixer control '%s' not found, skipping\n", name);
    else
        printf("audio: mixer control '%s' is type %d, not %s, skipping\n",
            name, type, what);
    return false;
}

static void nwa30_set_bool_if_present(const char *name, bool val)
{
    if(nwa30_control_is(name, SND_CTL_ELEM_TYPE_BOOLEAN, "a switch"))
        alsa_controls_set_bool(name, val);
}

static void nwa30_set_enum_if_present(const char *name, const char *value)
{
    if(nwa30_control_is(name, SND_CTL_ELEM_TYPE_ENUMERATED, "an enum"))
        alsa_controls_set_enum(name, value);
}

/* /proc/asound/pcm lists six devices and only device 1 is on the "STD" DAI;
 * the rest are Sony's own "ICX". Their player is 10dB louder at the same codec
 * volume, so which device it really opens is worth trying rather than arguing
 * about - and each attempt would otherwise cost a reflash:
 *
 *     echo hw:0,4 > /contents/.rockbox/pcm_device.txt
 *
 * Delete the file to go back to the built-in default. */
static void nwa30_apply_pcm_override(void)
{
    FILE *f = fopen(PCM_DEVICE_OVERRIDE_FILE, "re");
    if(f == NULL)
    {
        printf("audio: pcm device %s (no override file)\n",
            DEFAULT_PLAYBACK_DEVICE);
        return;
    }
    static char device[32];
    if(fgets(device, sizeof(device), f) != NULL)
    {
        device[strcspn(device, " \t\r\n")] = 0;
        if(device[0] != 0)
        {
            pcm_alsa_set_playback_device(device);
            printf("audio: pcm device %s (from pcm_device.txt)\n", device);
        }
    }
    fclose(f);
}

static void nwa30_list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if(dir == NULL)
    {
        printf("audio: cannot list %s\n", path);
        return;
    }
    printf("--- %s ---\n", path);
    struct dirent *entry;
    while((entry = readdir(dir)))
        if(entry->d_name[0] != '.')
            printf("  %s\n", entry->d_name);
    printf("--- end of %s ---\n", path);
    closedir(dir);
}

static void nwa30_print_file(const char *path, int max_lines)
{
    FILE *f = fopen(path, "re");
    if(f == NULL)
    {
        printf("audio: no %s\n", path);
        return;
    }
    printf("--- %s ---\n", path);
    char line[256];
    int lines = 0;
    while(fgets(line, sizeof(line), f) != NULL && lines++ < max_lines)
        fputs(line, stdout);
    printf("--- end of %s (%d lines) ---\n", path, lines);
    fclose(f);
}

/* The volume curve lives in the codec driver: Sony loads a model-specific table
 * and "master volume" is an index into it, so the mapping has to be read off
 * the device rather than guessed. Which PCM is which decides where the sound
 * goes, and hw:0,1 was an early guess while the stock HAL knows hw:0,0 to
 * hw:0,4. Print what the card actually offers. */
static void nwa30_dump_audio_info(void)
{
    static const char *paths[] =
    {
        "/proc/asound/pcm",
        "/proc/asound/cards",
        CODEC_PROC_DIR,
        CODEC_PROC_DIR "/ovt",
        CODEC_PROC_DIR "/hvt",
    };
    nwa30_list_dir(CODEC_PROC_DIR);
    for(unsigned i = 0; i < ARRAYLEN(paths); i++)
        nwa30_print_file(paths[i], 200);
    fflush(stdout);
}

/* Where the stock firmware leaves the output gain, which is its maximum and
 * where the loudness ceiling comes from. An earlier build drove this to zero
 * for "low power" and left players silent, so set it rather than trust it. */
static void nwa30_restore_output_gain(void)
{
    long lo, hi;
    if(!alsa_controls_get_range("master gain", &lo, &hi))
        return;
    alsa_controls_set_ints("master gain", 1, &hi);
    printf("audio: 'master gain' set to %ld (max of %ld..%ld)\n", hi, lo, hi);
}

/* Mixer settings applied last, read from MIXER_OVERRIDE_FILE - one per line,
 * "control name = value", '#' comments out a line:
 *
 *     headphone amp = smaster-btl
 *     master gain = 20
 *     Master Switch = 1
 *
 * This player has a few dozen mixer controls whose meaning is documented
 * nowhere, and testing one of them otherwise costs a build, a transfer and a
 * restart. Integers and booleans are set as numbers, enumerations by the name
 * the driver lists for them. */
static void nwa30_apply_mixer_overrides(void)
{
    FILE *f = fopen(MIXER_OVERRIDE_FILE, "re");
    if(f == NULL)
        return;
    char line[128];
    while(fgets(line, sizeof(line), f) != NULL)
    {
        char *hash = strchr(line, '#');
        if(hash)
            *hash = 0;
        char *eq = strchr(line, '=');
        if(eq == NULL)
            continue;
        *eq = 0;
        char *name = line, *value = eq + 1;
        /* trim both sides of both halves */
        while(*name == ' ' || *name == '\t')
            name++;
        for(char *p = eq - 1; p >= name && (*p == ' ' || *p == '\t'); p--)
            *p = 0;
        while(*value == ' ' || *value == '\t')
            value++;
        value[strcspn(value, "\r\n")] = 0;
        for(char *p = value + strlen(value) - 1;
            p >= value && (*p == ' ' || *p == '\t'); p--)
            *p = 0;
        if(*name == 0 || *value == 0)
            continue;

        switch(alsa_controls_get_type(name))
        {
            case SND_CTL_ELEM_TYPE_INTEGER:
            {
                long v = strtol(value, NULL, 0);
                alsa_controls_set_ints(name, 1, &v);
                printf("audio: mixer.txt: '%s' = %ld\n", name, v);
                break;
            }
            case SND_CTL_ELEM_TYPE_BOOLEAN:
                alsa_controls_set_bool(name, strtol(value, NULL, 0) != 0);
                printf("audio: mixer.txt: '%s' = %s\n", name,
                    strtol(value, NULL, 0) ? "on" : "off");
                break;
            case SND_CTL_ELEM_TYPE_ENUMERATED:
                alsa_controls_set_enum(name, value);
                printf("audio: mixer.txt: '%s' = '%s'\n", name, value);
                break;
            default:
                printf("audio: mixer.txt: no control '%s', skipping\n", name);
                break;
        }
    }
    fclose(f);
    fflush(stdout);
}
#endif

void audiohw_set_playback_src(enum nwz_src_t src)
{
#ifdef SONY_NWA30
    /* The CXD3778GF has no source multiplexer: PCM playback always goes
     * through the digital path, while the tuner is an analog input that is
     * mixed in separately, muted by "analog playback mute".
     *
     * Which input that is gets picked with "analog input device", but that one
     * is an enumeration (the device rejected being set as a switch) and we do
     * not know its values yet, so leave it alone: whatever the system left it
     * as is fine as long as the analog path stays muted for playback. Reading
     * the items back is what FM radio support will need. */
    switch(src)
    {
        case NWZ_RADIO:
            nwa30_set_bool_if_present("analog playback mute", false);
            break;
        case NWZ_MIC: /* not wired up on this platform */
        case NWZ_PLAYBACK:
        default:
            nwa30_set_bool_if_present("analog playback mute", true);
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
#ifdef SONY_NWA30
    /* Use the raw "hw:0" control interface rather than "default": this runs
     * during boot and alsa_controls_init() panics if the device cannot be
     * opened. "default" is resolved through the alsa-lib configuration files
     * (/usr/share/alsa), which may be absent on this player, whereas "hw" is
     * built into alsa-lib and always opens as long as the card exists (it does,
     * card 0 "sonysoccard", confirmed by recon). Functionally identical for
     * enumerating and setting controls. */
    alsa_controls_init("hw:0");
#else
    alsa_controls_init("default");
#endif
#ifdef SONY_NWA30
    /* The names were read out of the device's kernel but not their types, so
     * put the real list in the log to work from. */
    alsa_controls_dump();
    nwa30_dump_audio_info();
    nwa30_apply_pcm_override();
    nwa30_restore_output_gain();
    /* The stock audio HAL clears several mutes to start playback and we had
     * only cleared one; "headphone mute" in particular stayed on, so even with
     * the output routed to the jack nothing came through. "analog playback
     * mute" is deliberately left alone - it belongs to the tuner input and must
     * stay muted for playback, see audiohw_set_playback_src(). */
    nwa30_set_bool_if_present("playback mute", false);
    nwa30_set_bool_if_present("digital playback mute", false);
    nwa30_set_bool_if_present("headphone mute", false);
    /* "Master Switch" is deliberately not touched. It belongs to the SoC mixer,
     * which sits ahead of the codec, and the control dump taken before we set
     * anything shows the driver leaves it at 0 - the state the stock firmware
     * plays in, since nothing in /system writes it either. We used to set it
     * true while hunting silence, and that is the one gain stage we were
     * driving differently from the stock firmware. */
    /* Before routing anything to the amplifier, not after: it powers up on
     * 'normal' and changing it later - which is what the "DAC power mode"
     * setting did - made no audible difference, as though the value had been
     * latched at power-up. The setting can still switch it afterwards. */
    nwa30_set_enum_if_present("headphone smaster se gain mode", "high");
    nwa30_set_enum_if_present("headphone smaster gain mode", "high");
    nwa30_set_enum_if_present("headphone smaster btl gain mode", "high");
    /* Route the output to the headphone jack and turn its amp on. Without this
     * "output device" sits at 'off' and nothing is heard. The A35 has a
     * single-ended 3.5mm jack, so use the S-Master SE amp; values confirmed
     * from the device's own control list and the stock HAL. */
    nwa30_set_enum_if_present("output device", "headphone");
    nwa30_set_enum_if_present("headphone amp", "smaster-se");
    /* and say what it reads back, so the next log shows whether the ordering
     * was what mattered */
    {
        long v = 0;
        if(alsa_controls_get_type("headphone smaster se gain mode") ==
           SND_CTL_ELEM_TYPE_ENUMERATED)
        {
            alsa_controls_get("headphone smaster se gain mode",
                              SND_CTL_ELEM_TYPE_ENUMERATED, 1, &v);
            printf("audio: se gain mode after routing reads %ld\n", v);
        }
    }
    /* make sure the tuner passthrough is not mixed into playback */
    audiohw_set_playback_src(NWZ_PLAYBACK);
    /* Keep the hardware "master volume" at its maximum and do the actual
     * attenuation digitally, see audiohw_set_volume(). Whether the control is
     * mono or stereo is not documented, so ask the driver how many values it
     * takes instead of assuming. */
    if(alsa_controls_get_type("master volume") == SND_CTL_ELEM_TYPE_INTEGER)
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
    /* last, so anything above can be overridden without a rebuild */
    nwa30_apply_mixer_overrides();
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

/* fp_factor() turns decibels into an amplitude ratio; this goes the other way,
 * so we can tell how much attenuation the hardware really placed and leave the
 * remainder to the digital volume. A search rather than a logarithm: it runs
 * only when the volume moves, and saves pulling in a log function. */
static int factor_to_tenth_db(long factor)
{
    int lo = -1000, hi = 40, best = -1000;
    while(lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if(fp_factor(fp_div(mid, 10, 16), 16) <= factor)
        {
            best = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return best;
}

/* The codec's own volume control. Not "Master Volume", which is the SoC
 * mixer's and sits before it. */
#define NWA30_VOLUME_CTL "master volume"
/* the range Rockbox asks for, in tenth-dB. Mirrors the AUDIOHW_SETTING in
 * firmware/export/nwzlinux_codec.h, which declares -100..+4 dB. */
#define NWA30_VOL_MIN (-1000)
#define NWA30_VOL_MAX (40)

void audiohw_postinit(void)
{
}

#ifdef SONY_NWA30
/* Everything audiohw_set_volume() needs to know about the hardware control,
 * probed once. A dB scale is the accurate way to place the volume; without one
 * we fall back to treating the control as linear in amplitude, which is what
 * Sony's drivers do - the older players' code says as much. */
static struct
{
    bool probed;
    bool usable;
    bool has_db;
    long min, max;
    long min_mdb, step_mdb;
} hw_vol;

static void probe_hw_volume(void)
{
    if(hw_vol.probed)
        return;
    hw_vol.probed = true;
    hw_vol.usable = alsa_controls_get_range(NWA30_VOLUME_CTL, &hw_vol.min,
                                            &hw_vol.max) &&
                    hw_vol.max > hw_vol.min;
    hw_vol.has_db = hw_vol.usable &&
                    alsa_controls_get_db_range(NWA30_VOLUME_CTL,
                                               &hw_vol.min_mdb,
                                               &hw_vol.step_mdb) &&
                    hw_vol.step_mdb > 0;
    if(hw_vol.has_db)
        printf("audio: '%s' %ld..%ld = %ld..%ld dB in %ld.%02ld dB steps\n",
            NWA30_VOLUME_CTL, hw_vol.min, hw_vol.max, hw_vol.min_mdb / 100,
            (hw_vol.min_mdb + (hw_vol.max - hw_vol.min) * hw_vol.step_mdb) / 100,
            hw_vol.step_mdb / 100, hw_vol.step_mdb % 100);
    else if(hw_vol.usable)
        printf("audio: '%s' %ld..%ld, no dB scale published - mapping %d..%d dB onto it\n",
            NWA30_VOLUME_CTL, hw_vol.min, hw_vol.max,
            NWA30_VOL_MIN / 10, NWA30_VOL_MAX / 10);
    else
        printf("audio: no '%s' control, keeping volume in software\n",
            NWA30_VOLUME_CTL);
    fflush(stdout);
}

/* Pick the control step for a request in tenth-dB, and report what that step is
 * actually worth so the caller can leave the remainder to the digital volume. */
static long hw_volume_step(int vol, long *placed_tenth_db)
{
    long step;
    if(hw_vol.has_db)
    {
        /* the request is in tenth-dB, the TLV in hundredths */
        step = (vol * 10 - hw_vol.min_mdb + hw_vol.step_mdb / 2) /
               hw_vol.step_mdb + hw_vol.min;
        step = MAX(hw_vol.min, MIN(hw_vol.max, step));
        *placed_tenth_db = (hw_vol.min_mdb +
                            (step - hw_vol.min) * hw_vol.step_mdb) / 10;
        return step;
    }
    /* Linear in amplitude, so spreading our dB range evenly over the steps put
     * nearly the whole audible range into the first few of them - a touch above
     * minimum was already loud. Convert to the amplitude ratio the requested
     * attenuation works out to, rounding up so the hardware never attenuates
     * past what was asked and the digital volume only ever takes a little more
     * off rather than putting any back. */
    long factor = fp_factor(fp_div(vol, 10, 16), 16); /* 1<<16 = 0dB */
    long span = hw_vol.max - hw_vol.min;
    if(factor >= (1 << 16))
        step = hw_vol.max;
    else
        step = hw_vol.min + ((span * factor + (1 << 16) - 1) >> 16);
    /* Below roughly -40dB no step is left to land on, and they are coarse well
     * before that, so the difference goes to the digital volume. It costs bits
     * only where there is headroom to spare. */
    *placed_tenth_db = factor_to_tenth_db(
        span ? fp_div(step - hw_vol.min, span, 16) : (1 << 16));
    return step;
}
#endif /* SONY_NWA30 */

#ifndef SONY_NWA30 /* the NW-A30 volume path is purely digital, see below */
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
#endif /* !SONY_NWA30 */

/* volume is in tenth-dB */
void audiohw_set_volume(int vol_l, int vol_r)
{
#ifdef SONY_NWA30
    /* Prefer the codec's own volume: attenuating in software throws away bits -
     * at -43dB, where the digital range ends, only about a third of the DAC's
     * 16 are left - while the hardware attenuates after the converter and keeps
     * them all. The curve itself is not ours to reproduce, Sony loads a
     * model-specific table into the codec, so the step comes from what ALSA
     * publishes about the control rather than from what "0..120" might mean. */
    const int min_pcm = -430;
    const int max_pcm = 0;
    probe_hw_volume();
    if(hw_vol.usable)
    {
        /* one value covers both channels, so place the louder in hardware and
         * leave any balance to the digital volume below */
        int vol = MAX(vol_l, vol_r);
        long placed_tenth_db;
        long step = hw_volume_step(vol, &placed_tenth_db);
        alsa_controls_set_ints(NWA30_VOLUME_CTL, 1, &step);
        vol_l -= placed_tenth_db;
        vol_r -= placed_tenth_db;
        /* The stock HAL drives only "master volume", "headphone amp" and
         * "output device", so at hw_vol.max we are at the same ceiling it has.
         * Report where each request lands, so "not loud enough" can be checked
         * against what was set rather than argued about. */
        printf("audio: volume %d.%d dB -> %s %ld/%ld, digital %d.%d dB\n",
            vol / 10, abs(vol) % 10, NWA30_VOLUME_CTL, step, hw_vol.max,
            vol_l / 10, abs(vol_l) % 10);
        fflush(stdout);
    }
    vol_l = MAX(min_pcm, MIN(max_pcm, vol_l));
    vol_r = MAX(min_pcm, MIN(max_pcm, vol_r));
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

#ifdef SONY_NWA30
/* high/low gain for the S-Master amplifier, offered as Rockbox's "DAC power
 * mode". Which control carries it depends on how the output is wired, so set
 * whichever of the three the device turns out to have; the ones that are
 * absent are skipped. */
void audiohw_set_power_mode(int mode)
{
    bool high = (mode != SOUND_LOW_POWER);
    const char *gain = high ? "high" : "normal";
    printf("audio: gain mode -> %s\n", gain);
    nwa30_set_enum_if_present("headphone smaster se gain mode", gain);
    nwa30_set_enum_if_present("headphone smaster gain mode", gain);
    nwa30_set_enum_if_present("headphone smaster btl gain mode", gain);

    /* Do NOT touch "master gain" here. It sits at its maximum already - which
     * is why switching the amplifier's range changes nothing audible, the
     * output is as loud as this hardware goes - and driving it down for "low"
     * simply muted the player. Report it instead. */
    long lo, hi, now = 0;
    if(alsa_controls_get_range("master gain", &lo, &hi))
    {
        alsa_controls_get("master gain", SND_CTL_ELEM_TYPE_INTEGER, 1, &now);
        printf("audio: 'master gain' %ld..%ld reads %ld (left alone)\n",
            lo, hi, now);
    }
    fflush(stdout);
}
#endif

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
