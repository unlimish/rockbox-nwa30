/*
 * This config file is for the Sony NW-A30 series
 *
 * The NW-A30 (NW-A35/A36/A37) is based on the "Hagoromo" platform:
 * a MediaTek SoC (ARMv7) with a Mali GPU, running Linux 3.10 with an
 * Android-style userspace (init.rc). It is quite different from the older
 * icx-based players (NWZ-A10/NW-A20):
 * - 3.1" WVGA (480x800 portrait) TFT with capacitive touchscreen
 * - CXD3778GF audio codec (S-Master HX), different ALSA controls
 * - main UI app is /system/vendor/sony/bin/HgrmMediaPlayerApp (not SpiderApp)
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 125

#define MODEL_NAME   "Sony NW-A30 Series"

/* LCD dimensions: hx8379c panel, 480x800 portrait, confirmed by recon
 * (mtkfb: XRES=480 YRES=800) */
#define LCD_WIDTH  480
#define LCD_HEIGHT 800
/* sqrt(480^2 + 800^2) / 3.1 = 300 */
#define LCD_DPI 300

/* the mtkfb framebuffer is 32bpp (confirmed by recon: bits_per_pixel=32,
 * stride 1920 = 480*4). Must be set before including sonynwzlinux.h which
 * otherwise defaults to 16bpp RGB565. */
#define LCD_DEPTH  32
#define LCD_PIXELFORMAT XRGB8888

/* this device has a touchscreen */
#define HAVE_TOUCHSCREEN
#define HAVE_BUTTON_DATA

/* The himax controller reports positions in a 960x1600 space - exactly twice
 * the panel - so halve them to get pixels. Measured on the device: touching
 * the corners gives x up to 939 and y up to 1579. */
#define DEFAULT_TOUCHSCREEN_CALIBRATION { .A=1, .B=0, .C=0, \
                                          .D=0, .E=1, .F=0, \
                                          .divider=2 }

#define NWZ_HAS_SD

/* on the Hagoromo platform the main application lives in the vendor
 * partition, the dualboot installer renames it to .of */
#define NWZ_OF_APP  "/system/vendor/sony/bin/HgrmMediaPlayerApp.of"

/* This player's tuner is driven by radio_si4708icx, which does not implement
 * the ioctls radio-nwz.c uses (they come back ENOTTY on the device), so there
 * is no working FM radio here yet. */
#define NWZ_NO_TUNER

/* The CXD3778GF exposes several PCM devices; the normal (non-hi-res) music
 * playback path is card 0 device 1 (cxd3778gf-standard), confirmed by recon.
 * Device 0 is the hi-res-only output.
 *
 * Use the raw "hw" device instead of "plughw": snd_pcm_open() is called during
 * boot (pcm_init -> sink_dma_init) and panics on failure, and "plughw" is
 * resolved through the alsa-lib configuration (/usr/share/alsa) which may not
 * be present on this player. "hw" is built into alsa-lib and opens without any
 * config as long as the device node exists (pcmC0D1p, confirmed by recon). The
 * trade-off is that "hw" does no automatic format conversion, so if the codec
 * refuses Rockbox's format the audio may be wrong until the format is matched -
 * but set_hwparams() failures are non-fatal, so the player still boots. */
#define DEFAULT_PLAYBACK_DEVICE "hw:0,1"
#define DEFAULT_CAPTURE_DEVICE  "hw:0,1"

#include "sonynwzlinux.h"

/* override keypad: reuse the NWZ-A860 one (touchscreen + media keys) */
#undef CONFIG_KEYPAD
#define CONFIG_KEYPAD SONY_NWZA860_PAD

/* The rest of the family takes 32-bit samples, but this player's CXD3778GF
 * does not: asked what it accepts, hw:0,1 answers "S16_LE" and nothing else,
 * and set_hwparams() was failing at the format step every time playback
 * started - which looked like a track that ends the instant it begins. */
#undef HAVE_ALSA_32BIT

/* hw:0,1 is the codec's "standard" PCM and it really only runs at 44.1kHz.
 * It accepts a higher rate without complaining - snd_pcm_hw_params_set_rate_near()
 * hands back exactly what it was asked for - but the clock does not follow, so
 * the samples come out at 44.1kHz regardless: a 96kHz track played at 0.46x
 * speed, pitched down to match, and 48kHz was slightly flat for the same
 * reason. Claim only what the hardware does and let the DSP resample to it.
 * (The 88.2/96k and up rates the family header lists belong to pcmC0D0p, the
 * "hires" PCM, which we do not open yet.) */
#undef HW_SAMPR_CAPS
#define HW_SAMPR_CAPS   SAMPR_CAP_44

/* The older players leave USB entirely to the stock firmware, but here the
 * framework that would do it is frozen while Rockbox runs (see system-nwz.c),
 * so the player turns up as an empty drive and the only way to copy anything
 * onto it is to leave Rockbox first. We can drive the mass storage gadget
 * ourselves - init.usbcfg.rc grants "system" write access to the one file that
 * matters - so let the usb thread run and do it, see usb-nwz.c. */
#ifndef SIMULATOR
#undef USB_NONE
#endif

/* /dev/icx_power does not exist on this platform, so the ioctl-based battery
 * and charger readings the family uses always fail - the player has been
 * running on an "assume nominal" fallback with no real battery indicator. The
 * kernel's standard power supply class is there instead, which the generic
 * target/hosted/power-linux.c already speaks, so use that and report a real
 * percentage rather than a voltage guessed from a curve for a different cell. */
/* Rockbox ships no CJK font above 16px, so on a 480x800 screen every
 * Japanese filename and tag was either unreadable or a row of boxes. Ship a
 * 35px Noto Sans JP cut down to JIS X 0208 (kana, punctuation and the level
 * 1+2 kanji) - about 7000 glyphs and a 1.1MB .fnt, against tens of megabytes
 * for the full CJK repertoire. Regenerate with
 * utils/nwztools/scripts/make_cjk_font.py. */
#define DEFAULT_FONTNAME "35-Noto-Sans-CJK-JP"

/* The family header points the second drive at /mnt/media, which is where the
 * icx players mount a card. This one puts it on /contents_ext, as exfat
 * through FUSE - so the card was there and mounted, but Rockbox was looking
 * somewhere empty and only ever showed the internal memory. */
#undef MULTIDRIVE_DIR
#define MULTIDRIVE_DIR "/contents_ext"

/* Scanning "/" alone finds only the internal memory. The card shows up in the
 * root as a link, and tagcache follows links by calling readlink() on them -
 * but this one is not a link on disk, it is an entry Rockbox makes up for the
 * second volume, so readlink() fails and the card is quietly dropped. Name it
 * explicitly instead, so "Update now" covers both. */
#define DEFAULT_TAGCACHE_SCAN_PATHS "/:/<microSD1>"

#ifndef SIMULATOR
#undef CONFIG_BATTERY_MEASURE
#define CONFIG_BATTERY_MEASURE PERCENTAGE_MEASURE
#define BATTERY_DEV_NAME "battery"
#define POWER_DEV_NAME   "usb"
#endif
