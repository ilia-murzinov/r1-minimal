# R1 Minimal

I run a HiBy R1 as a plain DAP: music on a card, headphones, done. Stock
firmware is noisy. Full [Sonix](https://github.com/Jepl4r/sonix-player) is
wonderful and heavier than I need. This is the middle — Sonix, stripped to
what I actually touch.

![The five screens](docs/screens/banner.png)

Four tiles on the home screen. Now Playing for the track. Artists and Genres
to dig through the library (scan with a tap if the card is empty; long-press
to rescan). Settings for brightness, Bluetooth, and ADB — MSC is the default
so the card shows up as a disk when you plug in.

Charge still caps at HiBy’s stock 80%. No streaming, no Wi‑Fi, no EQ toys, no
second Settings planet.

### Put it on the player

1. Download [`r1.upt`](https://github.com/ilia-murzinov/r1-minimal/releases/tag/v0.1.0)
   from the release.
2. Copy it to the **root** of the microSD card.
3. `bootmode.sh Recovery` over ADB, then reboot and let Recovery flash it.

```text
build → packer → r1.upt on the card → Recovery → reboot
```

There’s also a soft bind over ADB (`deploy/`) for day-to-day hacking. It does
not survive a full power cycle — flash when you want it to stick.

Built on [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player)
(GPL-3.0). Thanks to Tartarus6, noisetta, endgame47, hkhrithik007, and the
rest of that crew.
