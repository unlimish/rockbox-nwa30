# Rockbox on the Sony NW-A30 (unofficial)

![Rockbox on three NW-A35 players: a track playing with album art, the boot
logo, and the main menu](docs/nwa30/nwa30-rockbox.png)

A work-in-progress Rockbox port to the Sony NW-A30 series (NW-A35/A36/A37),
Sony's "Hagoromo" platform: a MediaTek MT8127 running Linux 3.10 with a
hard-float glibc userspace.

This is **not** an official Rockbox port and is not submitted for inclusion
upstream. It is a fork of Rockbox with a new target added; everything outside
the NW-A30 target is upstream Rockbox, under its own licence (GPLv2).

## Provenance

The NW-A30 target here was written with heavy use of an AI assistant (Claude).
The upstream project asks that AI-generated contributions be explainable line by
line and does not generally accept them, which is why this lives in its own
repository rather than as a pull request. If any of it is ever useful upstream,
it should be reworked and defended by a person who understands it, not
submitted as-is.

## State

Working on the device:

- boots and runs for hours from the user partition, replacing the stock
  application
- 480x800 display at 32bpp, backlight control
- all buttons (rew, ff, play/pause, volume, power) and the hold switch
- touchscreen, calibrated
- audio playback, with the volume set on the codec rather than in software
- the memory card, as a second volume
- USB mass storage while Rockbox is running
- battery level from the kernel's power supply class
- Japanese text, from a font generated for this port

Not working yet:

- ~~output is roughly 10 dB quieter than the stock firmware~~ **fixed, and
  confirmed on the device.** The port had been playing through `hw:0,1`, which
  Sony's kernel source shows is the FM radio path rather than the music one.
  See "Sony publishes the source for this player".
- **Bluetooth.** Audio goes out through Sony's own stack, which is not something
  Rockbox has a counterpart for. Use the stock firmware for that.
- **Plugins are not built** (`plugins=""`): most have no keymap for this pad and
  stop the build. `db_folder_select.rock` being absent is the visible one.
- ~~FM radio~~ **works.** Tuning, stereo and signal strength come through the
  tuner's V4L2 node; see "The FM tuner" below. Set the region to **Japan (wide)**
  for 76-95MHz, which is what this player's band actually is.
- **Real power off and reboot.** `reboot(2)` needs a capability we do not have,
  so shutdown falls through to suspend and "Reboot" exits to the boot menu.
  Sony's own `force_power_off` was the obvious way round and it is **not open to
  us**: the attribute is 0644 and root's, and the device answers EACCES. The
  boot-reason flags next to it are world-readable and do work.

## Sony publishes the source for this player

`oss.sony.net` carries the GPL source for the page titled
[NW-WM1A/NW-WM1Z/NW-A37HN/NW-A36HN/NW-A35HN/**NW-A35**](https://oss.sony.net/Products/Linux/Audio/NW-WM1A.html) -
this player is named on it. `linux-kernel-3.10.26.tar.gz` is the kernel this
port runs on, and it contains `sound/soc/codecs/cxd3778gf/`, the driver behind
every mixer control this port pokes at.

Read it before guessing at anything audio. It settles in minutes questions this
port spent weeks on:

- **which PCM device is the music path** - see the table further down, this was
  the big one
- `master volume` is not an attenuator but an index into a table that writes
  four registers at once, `SDIN1VOL`, `SDIN2VOL`, `PLAYVOL` and `PHV_L/R`
- the table itself is loaded from userspace through
  `/proc/icx_audio_cxd3778gf_data/ovt`, which is mode 0600 and so unreadable to
  us; the copy in the source is a stub
- **the amp gain modes do nothing on this player.** `get_table_index()` picks
  the high-gain table for an A-series board whatever the mode says -
  `/* TYPE_A is fixed HG */`
- `0x33` is the mute value for the volume registers, and `0x00` is 0 dB

It named what looked like a way to turn the player off, and then settled it the
other way. `drivers/power/icx_pm_helper.c` registers a platform device with a
`force_power_off` attribute that calls `mt_power_off()` - but the attribute is
created 0644 and owned by root, and the device duly answers EACCES. It is still
tried before falling back to suspend, since a failed `open()` costs nothing and
the log then says plainly which wall we are at. The same device publishes why
the machine last booted, split into flags - `boot_powerkey`, `boot_wdt`,
`boot_deadbat`, `boot_thermal` and friends - and those *are* world-readable,
which beats parsing `bootreason=` out of the kernel command line.

**Several of the drivers this player loads are missing from the release.** Of
the modules in `/proc/modules`, only the audio codec and the `icx_usb*` headers
are there; `radio_si4708icx`, `wm_key`, `mtk_stp_bt_soc`, `cxd3778gf_dnc_core`
and `icx_nvp_emmc` have no source in the tarball. `icx_si4708.h` exists but
holds nothing except a reset GPIO. So the source does **not** help with:

- **FM radio.** Sony's `radio_si4708icx` is absent. Their *board* file is not,
  though, and it gives away enough to go round the driver - see below.
- **Bluetooth.** `mtk_stp_bt_soc` is not in the tree either, and it would not be
  enough regardless: BT audio needs a host stack and an SBC encoder above the
  transport, and Rockbox has neither.

### The FM tuner

Not working, but no longer a black box. Three things are now known and none of
them needed the missing driver.

**Where the chip is.** `arch/arm/mach-mt8590/icx_radio_i2c_devs.c` registers it
as `Si4708icx` on **i2c bus 2, address 0x10**, with **GPIO278** as reset. The
Si4708 is a Silicon Labs Si470x, whose register map is published - and the
mainline `si470x` driver is in the same tarball as a working reference for the
power-up and tuning sequence. So the tuner can be driven directly through
`/dev/i2c-2` without Sony's module, if i2c-dev is present and lets uid `system`
at the bus. Startup reports whether it does.

**How its audio gets out.** It is not a PCM stream. `switch_tuner()` in
`cxd3778gf_control.c` takes the tuner in on the codec's **AIN2** analog input,
through PGA2 at +3dB into ADC2. In ALSA terms that is `analog input device` =
`tuner`, and `analog playback mute` - which playback deliberately leaves on -
is what silences it.

**Why device 1 exists.** The STD DAI carries the stream the codec driver calls
"FM Playback", 44.1kHz and S16_LE only. Those limits belong to the tuner path;
believing they were the player's is what kept this port on the quiet device for
weeks.

Nor does it cover the userspace: `HgrmMediaPlayerApp`,
`libaudiohal-adleralsa.so`, `libVolumeServiceFw.so` and the hagodaemon IPC are
proprietary, so everything under "The Sony daemons" below stays reverse
engineered.

## Taking a screenshot

Hold **both volume keys** together. The picture goes to `dump_0001.bmp` at the
root of the user partition, next to `rockbox.log`, so it is there the next time
the player is plugged in.

Rockbox's own screenshot mechanism - the debug menu's "Screendump", which arms a
dump for the next USB insertion - is compiled out of `APPLICATION` builds, and a
cable is the one thing this player cannot spare: it is a request to the stock
framework to take the user partition away.

## How it fits together

### Who runs what

The dualboot installer renames the stock application and takes its place, so
init starts our bootloader believing it is starting the player. The bootloader
copies Rockbox somewhere it can be executed from, starts it in a session of its
own, and waits.

```mermaid
flowchart TD
    init["init<br/><i>root, reads init.rc</i>"]
    hgrm["/system/vendor/sony/bin/<b>HgrmMediaPlayerApp</b><br/><i>= our bootloader.elf after install</i>"]
    of["HgrmMediaPlayerApp<b>.of</b><br/><i>the stock player, renamed</i>"]
    menu{"boot menu"}
    stage["stage into /tmp/rockbox<br/><i>binary, .so libraries, codecs</i>"]
    rb["<b>rockbox.sony</b><br/><i>own session, survives our death</i>"]
    tools["service menu / scripts"]

    init -->|"starts, as uid 100 (system)"| hgrm
    hgrm --> menu
    menu -->|ROCKBOX| stage --> rb
    menu -->|OF| of
    menu -->|TOOLS| tools
    rb -.->|"exits (System -> Reboot)"| menu
```

Rockbox outlives the bootloader on purpose. The stock userspace kills the
application in that slot about half a minute in, and restarts it - which is our
bootloader again, so it checks for a Rockbox that is already running and stands
aside rather than starting a second one.

The first thing Rockbox itself draws is the logo, with the commit it was built
from underneath - the middle player at the top of this page. That line is worth
reading before drawing any conclusion from a test: a stale build looks exactly
like a broken one, and this port lost whole evenings to that.

### What Rockbox links, and what it steps around

The stock player reaches the same codec through Sony's own HAL. We do not use
any of it; we open ALSA directly and drive the mixer ourselves. The two columns
below never meet at runtime, because the stock player is not running when
Rockbox is.

```mermaid
flowchart TB
    subgraph rockbox["Rockbox"]
        app["rockbox.sony"]
        pcm["pcm-alsa.c<br/><i>PCM stream, digital volume</i>"]
        ctl["alsa-controls.c<br/><i>mixer by name</i>"]
        codecs[".codec plugins<br/><i>dlopen from /tmp/rockbox/codecs</i>"]
    end

    subgraph sony["Stock firmware (idle while Rockbox runs)"]
        hal["libaudiohal-adleralsa.so<br/><i>master volume, headphone amp,<br/>output device</i>"]
        vol["libVolumeServiceFw.so / libVolumeGlue.so<br/><i>balance, amp gain modes</i>"]
    end

    asound["libasound.so.2<br/><i>shipped in .rockbox/lib</i>"]
    snd["/dev/snd/pcmC0D1p + control<br/><i>hw:0,1 = cxd3778gf-standard, the STD DAI</i>"]
    codec["<b>CXD3778GF</b><br/><i>S-Master HX, S16_LE, 44.1kHz</i>"]

    app --> pcm --> asound
    app --> ctl --> asound
    app --> codecs
    asound --> snd --> codec
    hal --> snd
    vol --> snd

    style sony stroke-dasharray: 5 5
```

Which of the card's six PCM devices to open is the single question that has cost
this port the most, and it was answered by reading Sony's kernel source rather
than by any amount of listening. `sound/soc/mediatek/mt8590/icx-machine-links.c`
wires device 0 to the codec's ICX DAI and device 1 to its STD DAI, and
`sound/soc/codecs/cxd3778gf/cxd3778gf.c` names their streams:

| device | DAI | stream | takes |
| --- | --- | --- | --- |
| `hw:0,0` `cxd3778gf-hires-out` | ICX | `Playback` | 5512-384000 Hz, S16/S24/S32/DSD |
| `hw:0,1` `cxd3778gf-standard` | STD | **`FM Playback`** | 44100 Hz only, S16_LE only |

Device 0 is not a hi-res-only output despite the name - it is the playback path,
which also carries hi-res. **Device 1 is the FM radio.** This port spent months
playing music through the tuner's input, and everything it concluded about the
hardware from that - that the codec takes `S16_LE` and nothing else, that it
really runs at 44.1kHz whatever it claims - was true of the tuner path and of
nothing else.

### The Sony daemons

Every one of Sony's daemons is a `hagodaemon` process carrying `libpstcore.so`,
which watches the foreground application and restarts the machine when it
decides one is stuck - as it would for us, since we do not speak the IPC it
waits on. So Rockbox freezes them with `SIGSTOP` at startup and `SIGCONT`s them
on the way out.

Freezing all of them stops the restarts but also takes USB with it, because the
chain that carries a cable insertion runs through them. These stay awake:

```mermaid
flowchart TD
    cable(["USB cable"]) --> kern["kernel<br/><i>power_supply/usb/online</i>"]
    kern --> wm["<b>WMPortService</b><br/><i>the WM-PORT connector</i>"]
    wm --> ev["<b>EventRouter</b><br/><i>delivers the event</i>"]
    ev --> fm["<b>FuncMgrServiceFw</b><br/><i>picks the mode</i>"]
    ev --> cm["<b>ConnMgrServiceFw</b><br/><i>drives the switch</i>"]
    fm --> usbm["<b>UsbMgrServiceFw</b>"]
    cm --> usbm
    usbm --> uhc["<b>UsbHostConnectionService</b>"]
    usbm --> sm["<b>StorageMgrServiceFw</b>"]
    sm --> prop["init: sys.sony.config = msc<br/><i>unmount_msc1, then the gadget LUN</i>"]
    prop --> host(["host sees a drive"])

    rb["Rockbox"] -.->|"closes its files<br/>and waits"| prop

    style rb stroke-dasharray: 5 5
```

Rockbox does not touch any of it. It cannot: `sys.sony.config` and `ctl.start`
are both refused for uid `system`, and `init.svc.unmount_msc1` reads back empty,
proving init never ran the service on our behalf. Writing to the gadget anyway
is what produced the long-standing symptom of a first cable that worked and a
second that came up empty - our writes left the framework's state machine out of
step with itself. All Rockbox owes it is to stop using the disk.

Two daemons are treated specially, in the other direction:

| daemon | what happens | why |
| --- | --- | --- |
| `appmgrservice` | frozen | it runs the timeout that restarts the machine when the home application never reaches the foreground |
| `PathMgrServiceFw` | frozen, and **must stay** frozen | sparing it makes the machine restart |
| `icx_bootanimation` | killed | nothing tells the boot splash that boot is over, so it keeps drawing over the screen |

Extra names can be spared without rebuilding by listing them one per line in
`/contents/.rockbox/usb_spare.txt`.

### What we are allowed to do

Rockbox runs as uid 100 (`system`), in group 3003 and nothing else. That single
fact explains most of the shape of this port:

```mermaid
flowchart TD
    uid["uid 100 (system)<br/>no CAP_SYS_ADMIN, no CAP_SYS_BOOT"]

    uid --> um["umount(2) → EPERM"]
    uid --> rb2["reboot(2) → EPERM"]
    uid --> susp["/sys/power/state → EPERM"]
    uid --> prop2["property writes silently refused<br/><i>setprop still exits 0</i>"]

    um --> um2["USB is the framework's job<br/><i>we release files and wait</i>"]
    rb2 --> rb3["'Reboot' exits to the boot menu<br/>instead of restarting"]
    susp --> susp2["power off falls through to suspend"]
    prop2 --> prop3["read the property back;<br/>never trust the exit code"]
```

### Where the files live

`/contents` is the user partition Rockbox sees as `/`. It is FAT32 and mounted
**noexec**, which is why nothing can be executed or `dlopen`'d from it.

```mermaid
flowchart LR
    subgraph disk["on disk"]
        contents["<b>/contents</b> (FAT32, noexec)<br/>= Rockbox '/'<br/><i>.rockbox, music, rockbox.log</i>"]
        ext["<b>/contents_ext</b> (exFAT via FUSE)<br/>= &lt;microSD1&gt;"]
    end

    subgraph tmp["/tmp (tmpfs, 32MB)"]
        stage["/tmp/rockbox<br/><i>rockbox.sony, *.so</i>"]
        cod["/tmp/rockbox/codecs"]
    end

    contents -->|"copied by the bootloader,<br/>before exec"| stage
    contents -->|"same, and only there"| cod
    stage -->|"LD_LIBRARY_PATH"| run(["running player"])
    cod -->|"dlopen, via lc-unix.c"| run
    contents --> run
    ext --> run
```

The codecs have to be staged by the *bootloader* rather than by Rockbox: a
running `rockbox.sony` may create directories under `/tmp/rockbox` but not
files, even in the directory the bootloader wrote the binary into moments
earlier. Loading an already-staged library works, so what is denied is file
creation, not dynamic loading.

The database needs the card named explicitly - `DEFAULT_TAGCACHE_SCAN_PATHS` is
`/<microSD1>:/`, card first - because tagcache drops a search root that lies
inside one it already holds, and a root of `/` is one character long, so it
swallows every later path. The card is not inside `/` here at all.

## Building

The toolchain is a hard-float ARMv7 one, which upstream `rockboxdev.sh` does not
build by default - the target added here (`--target=h`) does. A Docker recipe
that builds both it and the port is in `tools/docker_nwa30/`.

    docker build -f tools/docker_nwa30/Dockerfile.hf -t rockbox-nwa30:hf .
    mkdir -p build/nwa30 && cd build/nwa30
    ../../tools/configure --target=235 --type=n && make && make zip

**The device must get a hard-float binary.** A soft-float one does not exec at
all, and the failure looks exactly like a boot loop with an empty log, because
the application dies before it can open one. `readelf -l` before flashing.

The player needs `libasound.so.2` and the bootloader font shipped alongside the
binary in `.rockbox/`, since neither can be relied on to exist on the device.

Rockbox ships no CJK font above 16px, so this port generates its own:

    utils/nwztools/scripts/make_cjk_font.py \
        NotoSansJP.ttf 35 fonts/35-Noto-Sans-CJK-JP.bdf Regular

It cuts Noto Sans JP down to JIS X 0208 - kana, punctuation and the level 1+2
kanji, about 7000 glyphs - which keeps everyday Japanese readable without the
tens of megabytes the full repertoire would cost.

## Installing

`utils/nwztools/scripts/` has the firmware upgrade packages: `recon_hagoromo.sh`
dumps information about the device without changing it,
`install_dualboot_hagoromo.sh` installs the bootloader, and
`uninstall_dualboot_hagoromo.sh` puts the stock application back. Flashing needs
a Linux or Windows machine - `scsitool` has no macOS backend, and the official
macOS updater's kext is Intel-only.

**The stock application is renamed rather than deleted**, so the uninstall
package restores it; Sony's own firmware updater also restores everything.
