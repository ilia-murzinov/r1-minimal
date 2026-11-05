#!/bin/sh
# Bind userdata min over /usr/bin/sonix_player (root squashfs is RO).
BIN=/usr/data/sonix_player
MIN=/usr/data/sonix_player.min
[ -x "$MIN" ] || exit 1
cp -f "$MIN" "$BIN"
chmod 755 "$BIN"
umount /usr/bin/sonix_player 2>/dev/null || umount -l /usr/bin/sonix_player 2>/dev/null || true
mount --bind "$BIN" /usr/bin/sonix_player
