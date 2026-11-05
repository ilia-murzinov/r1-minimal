#!/bin/sh
LOG=/usr/data/watch_min.log
echo "$(date) watch start" >>"$LOG"
while true; do
  if ! pidof sonix_player >/dev/null 2>&1 && ! pidof sonix_player.min >/dev/null 2>&1; then
    echo "$(date) restart" >>"$LOG"
    if [ -x /usr/bin/sonix_launch ]; then
      start-stop-daemon -S -b -x /usr/bin/sonix_launch -- /usr/bin/sonix_player
    else
      start-stop-daemon -S -b -x /usr/bin/sonix_player
    fi
    sleep 4
  fi
  sleep 2
done
