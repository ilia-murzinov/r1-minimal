# Minimal R1 fork (on Sonix)

## Locked surface

Home 2×2: NOW/PLAYING · BLUE/TOOTH · ARTISTS · GENRES. No Wi‑Fi / QP / clock.
Empty library: tap Artists/Genres to scan; long-press either to rescan.
USB MSC by default; 10× taps on the Bluetooth header band toggles ADB.

## Gone from the tree

Streaming (Tidal/Qobuz/podcast/radio), audiobooks + chapters UI, Wi‑Fi /
AirPlay / DLNA / SonixLink, quickpanel, Gearboy/games/ebooks, and the
temporary `removed_stubs` / `removed_features` shim layer.

Sleep timer is music-only. LED has no podcast / Wi‑Fi-transfer patterns.

Also gone (unreachable from home, not compiled, not initialised at startup):
the whole Settings hub and its pages (music, power, appearance, boot logo,
screensaver, language, time, key remap, keyboard layouts, music tiles, artist
exceptions, system, processes, dev options), the EQ / PEQ / MSEB *pages*, the
Music grid, the file browser, search, the playlist page and the coverflow. The
first-boot welcome / language / clock chain is gone too: boot goes straight to
home. The track menu offers Queue / Show album / Details only. Audio/BT
engines are unchanged. Packer R1 image drops Flappy Bird assets and keeps
English only.

Saved power config (screen timeout, auto-off, standby, charge limit, LEDs,
key lock) is still applied at boot from the config file, with the defaults the
Settings pages used; there is just no page to change it.

## What runs

- Binary: `/usr/data/sonix_player.min` (~4.5 MB), also copied to `/usr/data/sonix_player`.
- Soft-restart: bind-mount over `/usr/bin/sonix_player` + `/usr/data/watch_min.sh`.
- **Full reboot:** root is squashfs (RO). Bind mounts die. Flash `r1.upt` from the packer so the min binary is *inside* the image. There is no Settings path any more: Recovery via `bootmode.sh` + `r1.upt` on the SD card.

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
# On device: Recovery via bootmode.sh with r1.upt on the SD card (no Settings path)
```
