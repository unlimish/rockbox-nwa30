#!/bin/sh

# Diagnostic wrapper installed in place of HgrmMediaPlayerApp.
#
# Round 1 established that the app IS started and does reach the exec of
# the real bootloader, and that /contents is mounted rw the whole time -
# yet the bootloader never writes a line to /contents/rockbox.log and the
# menu never appears. Two things stood out in that run:
#
#   - we run as uid=100(system), not root, with only group 3003(inet).
#     The bootloader SIGKILLs icx_bootanimation on startup; as a non-root
#     user that kill fails, so the wave animation keeps drawing over
#     whatever the bootloader puts on screen. That alone would look
#     exactly like "the bootloader never runs".
#   - /contents is mounted noexec, which is the same restriction that
#     makes dlopen() of the codecs fail.
#
# So this round captures the bootloader's own stdout/stderr (including
# anything the dynamic loader prints before main() runs, which never
# reaches its self-managed log file) and records who owns the processes
# involved.

LOG=/tmp/wrapper.log
OUT=/contents/wrapper.log
BL_OUT=/contents/bl_out.log

echo "=== wrapper: started ===" > "$LOG"
id >> "$LOG" 2>&1

echo "-- processes (looking for icx_bootanimation and its owner) --" >> "$LOG"
ps >> "$LOG" 2>&1

echo "-- can we see the framebuffer? --" >> "$LOG"
ls -l /dev/graphics/fb0 /dev/fb0 >> "$LOG" 2>&1

echo "-- rockbox.log before --" >> "$LOG"
ls -l /contents/rockbox.log >> "$LOG" 2>&1

cat "$LOG" >> "$OUT" 2>/dev/null
sync

# Hand over, but keep hold of the bootloader's output this time. If it
# dies in the dynamic loader, or panics before it opens its own log, this
# is the only place that message can land.
echo "=== bootloader output follows ===" >> "$BL_OUT" 2>/dev/null
sync
exec /system/vendor/sony/bin/rockbox_bl.real >> "$BL_OUT" 2>&1
