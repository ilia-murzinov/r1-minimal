#!/bin/sh
# Stage the min binary in userdata. Soft restart runs it via watch_min.sh
# directly (squashfs /usr/bin/sonix_player cannot be overwritten; binds die).
MIN=/usr/data/sonix_player.min
BIN=/usr/data/sonix_player
[ -x "$MIN" ] || exit 1
cp -f "$MIN" "$BIN"
chmod 755 "$BIN" "$MIN"
# Best-effort bind for anything that still execs /usr/bin/sonix_player.
umount /usr/bin/sonix_player 2>/dev/null || umount -l /usr/bin/sonix_player 2>/dev/null || true
mount --bind "$BIN" /usr/bin/sonix_player 2>/dev/null || true
