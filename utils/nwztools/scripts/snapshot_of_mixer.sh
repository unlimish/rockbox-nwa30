#!/bin/sh
# Record the stock firmware's mixer state while it is playing.
#
# Copy this and alsadump to the root of the user partition, then pick it from
# the bootloader's TOOLS -> Run script. It starts the stock firmware and, a
# minute and a half later, writes what the mixer looks like to
# /contents/of_mixer.txt. Play something at a normal volume in the meantime.
#
# Reboot into Rockbox afterwards and fetch of_mixer.txt over USB. Rockbox
# prints the same list at its own startup, so the two can be put side by side -
# which is the only way to see which gain stage the stock firmware sets and
# this port does not.
#
# Nothing here is installed: no reflash, nothing is left behind but the log.

STAGE=/tmp/alsadump
OUT=/contents/of_mixer.txt
DELAY=90

# /contents is mounted noexec, so the binary and the libraries it needs have to
# be run from tmpfs, the same way the bootloader starts Rockbox.
mkdir -p "$STAGE"
cp /contents/alsadump "$STAGE/alsadump" || exit 1
cp /contents/.rockbox/lib/* "$STAGE/" 2>/dev/null
chmod 755 "$STAGE/alsadump"

(
    sleep "$DELAY"
    LD_LIBRARY_PATH="$STAGE" "$STAGE/alsadump" > "$OUT" 2>&1
    sync
) &

# Hand the player over. This process is replaced, but the subshell above is a
# child of init now and outlives it.
exec /system/vendor/sony/bin/HgrmMediaPlayerApp.of
