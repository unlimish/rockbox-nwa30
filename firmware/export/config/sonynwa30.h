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

/* LCD dimensions */
/* NOTE: 480x800 portrait assumed, must be checked against the actual
 * framebuffer layout reported by the device (see recon script) */
#define LCD_WIDTH  480
#define LCD_HEIGHT 800
/* sqrt(480^2 + 800^2) / 3.1 = 300 */
#define LCD_DPI 300

/* this device has a touchscreen */
#define HAVE_TOUCHSCREEN
#define HAVE_BUTTON_DATA

#define NWZ_HAS_SD

/* on the Hagoromo platform the main application lives in the vendor
 * partition, the dualboot installer renames it to .of */
#define NWZ_OF_APP  "/system/vendor/sony/bin/HgrmMediaPlayerApp.of"

#include "sonynwzlinux.h"

/* override keypad: reuse the NWZ-A860 one (touchscreen + media keys) */
#undef CONFIG_KEYPAD
#define CONFIG_KEYPAD SONY_NWZA860_PAD
