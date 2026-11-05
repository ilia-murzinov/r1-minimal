# R1 Minimal

A personal **HiBy R1** cut of
[Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player).

Stock HiBy firmware and full Sonix are heavy for a local library player.
This fork keeps the bits that matter on an R1 used as a DAP with an SD card:
play music, browse the library, Bluetooth, and a short Settings page for USB
modes. Everything else is stripped so the UI stays small and B&W.

## What you get

Home is four tiles:

| | |
|---|---|
| **NOW PLAYING** | transport, queue, track details |
| **SETTINGS** | Brightness, Bluetooth + USB; **ADB** pinned at the bottom (MSC by default) |
| **ARTISTS** | artist → album → song |
| **GENRES** | genre → artist → album → song (albums are the artist’s full set) |

Empty library: tap Artists or Genres to scan; long-press either to rescan.

Charge stops at the stock **80%** limit (AXP REG64 ≈ 4.20 V). Bluetooth and
decode are upstream Sonix; graphic EQ / PEQ / MSEB are removed.

Gone: streaming, Wi‑Fi / AirPlay / DLNA, Settings hub (EQ, themes, language,
screensaver, …), browser, search, playlists UI, coverflow, games, ebooks.

## How it works

The player binary replaces HiBy’s on the stock R1 rootfs. The packer in
`sonix-packer/` unpacks a stock `r1.upt`, drops `sonix_player` into the
squashfs, and rebuilds `r1.upt`. Flash that image from Recovery (there is no
in-app firmware update page here).

```text
build binary → packer → r1.upt on microSD root → bootmode.sh Recovery → reboot
```

Soft deploy (ADB on, until reboot only): bind-mount the new binary over
`/usr/bin/sonix_player` via `deploy/bind_min.sh` + `deploy/watch_min.sh`.
A full power cycle needs a real `r1.upt` flash again.

USB: MSC for the card as a disk; flip **ADB** in Settings when you need a
shell. They stay exclusive.

## Credits

Upstream: [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player)
(GPL-3.0).

Special thanks to:

[@Tartarus6](https://github.com/Tartarus6)

[@noisetta](https://github.com/noisetta)

[@endgame47](https://github.com/endgame47)

[@hkhrithik007](https://github.com/hkhrithik007)

and all the members of this fantastic community!!
