# R1 Minimal

HiBy R1 player: local library on microSD, B&W UI, stock rootfs with
`sonix_player` replaced. Fork of [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player).

![UI](docs/screens/banner.png)

---

## Hardware buttons

| Button | Short press | Long press |
|---|---|---|
| **Power** | Screen on/off | Power menu: slide to power off, or slide to restart |
| **Vol +** | Volume up | Hold to repeat |
| **Vol −** | Volume down | Hold to repeat |
| **Play/Pause** | Play / pause | — |
| **Next** | Next track | — |

---

## Status bar

Always on (except full-bleed Now Playing artwork).

| Element | Meaning / action |
|---|---|
| Volume glyph + % | Current output level |
| Headphone jack glyph | Visible while a wired jack is plugged in |
| Play glyph | Something is loaded |
| Library glyph | Brief while a rescan runs |
| **Bluetooth glyph** | Always shown when BT hardware exists. Dim = off, medium = on with no link, full = connected. **Long-press** reconnects the last headphones (busy toast with **Cancel**). Tap does nothing — on/off lives in Settings |
| Battery % + zap | Charge percent; zap while charging |
| **Home** (centre) | Tap → home grid |

---

## Home (2×2)

| Tile | Tap | Long-press |
|---|---|---|
| **NOW PLAYING** | Opens the player sheet | — |
| **SETTINGS** | Brightness, Bluetooth, ADB | — |
| **ARTISTS** | Artist → album → song. Empty library → scan | Rescan (folder picker) |
| **GENRES** | Genre → artist → album → song. Empty → scan | Rescan |

While USB mass-storage owns the card (or no card), Artists/Genres toast instead
of opening the library.

Swipe from the left edge goes back one screen. Home icon in the status bar
jumps to the grid.

---

## Library

### First scan / rescan

1. Tap **ARTISTS** or **GENRES** with an empty library, **or** long-press either tile anytime.
2. Pick folders on the microSD to index.
3. Wait for the scan; the status bar shows a library glyph while it runs.

Music tree on the card: typically `Library/Artist/Album/…` (exFAT).

### Browse

- **Artists** → albums → tracks.
- **Genres** → artists → albums → tracks.
- Title-row corners: circle-play (play / shuffle the list), sort (A–Z ↔ Z–A; some lists also offer date or year).
- Tap a track to play it and continue from there.
- Long-press a row → selection mode: add to queue, favourite; leave with the close control.

---

## Now Playing

Open from the home tile, or from play.

| Control | Use |
|---|---|
| Cover / transport | Play/pause, prev, next |
| Progress bar | Seek |
| Queue position | e.g. `3/24` |
| Star | Favourite |
| Repeat / shuffle | Cycles: normal → repeat all → repeat one → shuffle → shuffle+repeat → normal |
| ⋮ (more) | **Queue**, **Show album**, **Details** (tags, format, path) |
| Swipe for lyrics | Drag the sleeve sideways to open lyrics |

### Lyrics

Looks for an `.lrc` next to the audio file, or under the card’s `Lyrics/` folder.
Names tried: same basename as the track, the title alone, or `Artist - Title.lrc`.

Example: `/mnt/sd_0/Lyrics/Artist - Title.lrc` (U-DISK: `HiBy/Lyrics/…`).

### Remember track

Always on in Minimal. After reboot the last track returns **paused at its
position**, with the queue restored. Open **NOW PLAYING** to continue.

---

## Settings

Home → **SETTINGS**.

### Brightness

Slider at the top. Value is saved when you release.

### Bluetooth

| Control | Use |
|---|---|
| **Bluetooth** switch | Radio on/off. Turning on also tries the last device |
| Status line | Local name / turning on / off |
| **Paired devices** | Tap a row → connect. Connected row → menu. ⋮ → disconnect / forget / rename. Long-press row → menu |
| Corner **search** | Scan for new devices; tap a found row to pair |
| Corner **receiver** | Bluetooth receiver mode (phone → R1). Disconnect headphones first |
| Corner **gear** | Bluetooth audio page: local name, absolute volume sync, codec (LDAC quality modes when available) |

**Status-bar long-press** (any screen): reconnect last device.

- Toast: **Connecting to \<name\>…** with **Cancel**
- Success: **Connected to \<name\>**
- Fail: pairing/connect help popup
- No last device: **No recent Bluetooth device**

### ADB

Footer toggle. Exclusive with mass-storage ownership of the card for library
access: when ADB is on you can `adb shell` / push; when off, plugging into a
computer exposes the microSD as a disk (U-DISK). Default for day-to-day music
copy is ADB **off** and MSC.

---

## Power menu

Long-press **Power** → slide **Power off** or **Restart**. Cancel by going
back / not completing the slide.

Short Power only blanks/wakes the panel.

---

## Charge

Plug USB with the unit off → charge screen (percent + zap). Hold **Power** to
boot into the player. Unplug while on the charge screen → power off.

---

## USB / computer

| Goal | How |
|---|---|
| Copy music / lyrics / covers | ADB **off**, plug in → microSD as disk (`HiBy`) |
| Shell / soft deploy | Settings → ADB **on**, then `adb devices` |
| Firmware update | Put `r1.upt` on SD **root**, boot recovery (below) |

---

## Flash

```text
build → sonix-packer → r1.upt on microSD root → recovery → reboot
```

On device (ADB):

```bash
adb push sonix-packer/r1.upt /mnt/sd_0/r1.upt
adb shell 'sync; sh /usr/bin/bootmode.sh Recovery && sync && reboot'
```

Or copy `r1.upt` to the SD root in U-DISK mode, then **Power + Vol Up** into
recovery and reboot.

Release asset: [`r1.upt`](https://github.com/ilia-murzinov/r1-minimal/releases/tag/v0.1.0).

Soft deploy (until reboot): `deploy/bind_min.sh` + `deploy/watch_min.sh`
over `/usr/bin/sonix_player`. A power cycle needs a real UPT flash.

---

## What Minimal deliberately drops

No EQ / PEQ / MSEB, no streaming / radio / ebooks / games / file manager hub,
no full stock Settings tree. Playback is local files + Bluetooth out (and BT
receiver when you open that page).

---

## Credits

Upstream [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player) (GPL-3.0).

[@Tartarus6](https://github.com/Tartarus6) ·
[@noisetta](https://github.com/noisetta) ·
[@endgame47](https://github.com/endgame47) ·
[@hkhrithik007](https://github.com/hkhrithik007)
