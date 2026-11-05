# Minimal R1 fork (on Sonix)

## Locked surface

Home 2×2: NOW/PLAYING · BLUE/TOOTH · ARTISTS · GENRES. No Wi‑Fi / QP / clock.
USB MSC by default; 10× Bluetooth title toggles ADB.

## What runs

- Binary: `/usr/data/sonix_player.min` (~5.5 MB), also copied to `/usr/data/sonix_player`.
- Soft-restart: bind-mount over `/usr/bin/sonix_player` + `/usr/data/watch_min.sh`.
- **Full reboot:** root is squashfs (RO). Bind mounts die. Flash `r1.upt` from the packer so the min binary is *inside* the image (Settings → System update from SD).

## Soft restart (no flash)

```sh
adb push .build-out/sonix_player /usr/data/sonix_player.min
adb push deploy/bind_min.sh deploy/watch_min.sh /usr/data/
adb shell 'sh /usr/data/bind_min.sh; killall watch_min.sh; start-stop-daemon -S -b -x /usr/data/watch_min.sh'
```

## Persistent firmware (survives power cycle)

```sh
cp .build-out/sonix_player sonix-packer/sonix_player
(cd sonix-packer && ./sonix_firmware_packer.sh)   # → r1.upt
adb push sonix-packer/r1.upt /mnt/sd_0/r1.upt
# On device: System → Firmware update → choose r1.upt → reboot
```
