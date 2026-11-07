# R1 Minimal

HiBy R1 fork of [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player).
Local library on microSD. B&W UI. Stock rootfs + replaced `sonix_player`.

![UI](docs/screens/banner.png)

## Scope

| | |
|---|---|
| **Home** | Now Playing · Settings · Artists · Genres |
| **Artists** | artist → album → song |
| **Genres** | genre → artist → album → song (album = that artist’s full set) |
| **Library** | tap Artists/Genres to scan; long-press to rescan |
| **Settings** | brightness, Bluetooth, USB; **ADB** at bottom (MSC default; exclusive with ADB) |
| **Charge** | stock 80% limit (AXP REG64 ≈ 4.20 V) |
| **Audio** | upstream Sonix decode + Bluetooth; no graphic EQ / PEQ / MSEB |

Removed: streaming, Wi‑Fi / AirPlay / DLNA, Settings hub, browser, search,
playlists UI, coverflow, games, ebooks.

## Flash

```text
build → sonix-packer → r1.upt on microSD root → bootmode.sh Recovery → reboot
```

Release asset: [`r1.upt`](https://github.com/ilia-murzinov/r1-minimal/releases/tag/v0.1.0).

Soft deploy (until reboot): `deploy/bind_min.sh` + `deploy/watch_min.sh`
bind-mounts over `/usr/bin/sonix_player`. Power cycle needs a real UPT flash.

USB: MSC = card as disk; Settings → ADB for shell. Mutually exclusive.

## Credits

Upstream [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player) (GPL-3.0).

[@Tartarus6](https://github.com/Tartarus6) ·
[@noisetta](https://github.com/noisetta) ·
[@endgame47](https://github.com/endgame47) ·
[@hkhrithik007](https://github.com/hkhrithik007)
