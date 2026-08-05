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

/* Music goes out of card 0 device 0, "cxd3778gf-hires-out".
 *
 * The name is misleading and cost this port a great deal: device 0 is not a
 * hi-res-only output, it is *the* playback path, which happens to also carry
 * hi-res. Sony's own kernel source settles it. The machine driver
 * (sound/soc/mediatek/mt8590/icx-machine-links.c) wires device 0 to the codec's
 * ICX DAI and device 1 to its STD DAI, and the codec driver names their streams:
 *
 *     ICX DAI  "Playback"      5512..384000 Hz, S16/S24/S32/DSD
 *     STD DAI  "FM Playback"   44100 Hz only, S16_LE only
 *
 * Device 1 is the FM radio path. Everything this port used to believe about the
 * hardware - that it takes S16_LE only and runs at 44.1kHz only - was measured
 * off that, and was a property of the tuner path rather than of the player.
 *
 * Capture stays on device 1, which is where the tuner actually is.
 *
 * Use the raw "hw" device instead of "plughw": snd_pcm_open() is called during
 * boot (pcm_init -> sink_dma_init) and panics on failure, and "plughw" is
 * resolved through the alsa-lib configuration (/usr/share/alsa) which may not
 * be present on this player. "hw" is built into alsa-lib and opens without any
 * config as long as the device node exists. Set /.rockbox/pcm_device.txt to try
 * another one without rebuilding. */
#define DEFAULT_PLAYBACK_DEVICE "hw:0,0"
#define DEFAULT_CAPTURE_DEVICE  "hw:0,1"

#include "sonynwzlinux.h"

/* override keypad: reuse the NWZ-A860 one (touchscreen + media keys) */
#undef CONFIG_KEYPAD
#define CONFIG_KEYPAD SONY_NWZA860_PAD

/* HAVE_ALSA_32BIT and the family's HW_SAMPR_CAPS both stand: the ICX DAI this
 * player's device 0 is wired to takes S16/S24/S32 and everything from 5512 to
 * 384000 Hz. They were both switched off here while the port was talking to
 * device 1, whose "FM Playback" stream really is S16_LE at 44100 and nothing
 * else - see the note on DEFAULT_PLAYBACK_DEVICE above. */


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

/* Scanning "/" alone finds only the internal memory, and the card has to be
 * named first or it is dropped again.
 *
 * tagcache refuses a search root that lies inside one it already has, and it
 * decides that with a plain strncmp() against the root's length
 * (search_root_exists() in tagcache.c). A root of "/" is one character long,
 * so *every* later path matches it and is thrown away - including the card,
 * which on this player is not inside "/" at all: Rockbox's "/" is /contents
 * and the card is /contents_ext. The same test also kills the automatic route,
 * where check_dir() meets the made-up <microSD1> link and offers /contents_ext
 * as a new root. (The code that would expand "/" into every volume is compiled
 * out for APPLICATION builds.)
 *
 * Putting the card first makes the length test work the other way round: "/"
 * does not start with "/<microSD1>", so it survives and both get scanned.
 * The card is then already a root when the link turns up, so it is not
 * scanned twice. */
#define DEFAULT_TAGCACHE_SCAN_PATHS "/<microSD1>:/"

#ifndef SIMULATOR
#undef CONFIG_BATTERY_MEASURE
#define CONFIG_BATTERY_MEASURE PERCENTAGE_MEASURE
#define BATTERY_DEV_NAME "battery"
#define POWER_DEV_NAME   "usb"
#endif
