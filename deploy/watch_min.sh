#!/bin/sh
# Keep the userdata min binary running. Avoid /usr/bin/sonix_player: that path
# is squashfs (RO) and boot/launch can undo a bind mount.
LOG=/usr/data/watch_min.log
MIN=/usr/data/sonix_player.min
echo "$(date) watch start" >>"$LOG"
while true; do
  if ! pidof sonix_player >/dev/null 2>&1 && ! pidof sonix_player.min >/dev/null 2>&1; then
    echo "$(date) restart $MIN" >>"$LOG"
    if [ -x /usr/bin/sonix_launch ]; then
      start-stop-daemon -S -b -x /usr/bin/sonix_launch -- "$MIN"
    else
      start-stop-daemon -S -b -x "$MIN"
    fi
    sleep 4
  fi
  sleep 2
done
