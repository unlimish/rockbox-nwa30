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
