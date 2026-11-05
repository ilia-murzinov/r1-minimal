# Minimal R1 fork (on Sonix, not from scratch)

Base: [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player).

## Locked surface

**Home (2×2):**

| | |
|--|--|
| NOW / PLAYING | BLUE / TOOTH |
| ARTISTS | GENRES |

No swipe-to-player. No quickpanel. No clock. No Wi‑Fi.

**USB / ADB:** mass storage by default; **10 taps on the Bluetooth title** toggles ADB.

## Deleted from tree (bootable checkpoint)

Streaming (Tidal/Qobuz/podcast/radio), audiobooks UI/DB, Wi‑Fi stack, AirPlay/DLNA/SonixLink sources, Game Boy / FlappyBird / ebooks, quickpanel/more/setup-wifi pages, online lyric search.

Now-playing still has dead branches behind temporary stubs (`removed_features.h`, `removed_stubs.c`, thin `net/`/`remote/` headers) so the binary boots while those call sites are cut.

Stripped binary ~5.5 MB (was ~9.1 MB).

## Build / flash

```bash
# Docker make target → .build-out/sonix_player
# push /usr/data/sonix_player.min ; start-stop-daemon restart
```
