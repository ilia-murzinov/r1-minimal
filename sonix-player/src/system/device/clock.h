#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <time.h>

// Wall clock handling.
//
// The player cannot assume the system time means anything. The device may have
// come up with the clock at the epoch, and unlike a phone there is nothing on
// board that will quietly correct it -- no network time, and the RTC only holds
// what was last written to it. So the time is something the user sets once, the
// player records that they did, and it puts the value back into the RTC so the
// next boot starts from the right place.

// True once a time has been set (now or in an earlier session). Until then the
// status bar shows no clock rather than a wrong one.
bool clock_is_set(void);

// Reads the saved state and, if the system clock came up obviously wrong,
// restores the last time the player saw. Call once at startup, after the
// config has been loaded.
void clock_init(void);

// Applies a user-chosen local date and time: sets the system clock, writes it
// through to the RTC so it survives a power cycle, and records in the config
// that the clock is now trustworthy. Returns false if the system clock could
// not be set (which needs root).
bool clock_set(int year, int month, int day, int hour, int minute);

// Writes the current time into the config so a device with no working RTC
// starts somewhere sensible instead of at the epoch. Cheap and idempotent;
// call it on a slow timer.
void clock_remember(void);

// ---------------------------------------------------------------------------
// The time zone
//
// By hand: chosen by the user, in Settings > Date and time > Time zone. Fixed
// offsets, with no daylight saving: switching zone moves the offset by an hour,
// exactly as the original firmware treats it.
//
// Automatic: located by the network along with the time, as an IANA zone whose
// rule moves between standard time and daylight saving on its own (see
// clock_zone_automatic()).
//
// Until one is chosen the player behaves as it always did -- the system clock
// holds local time and the C library shifts nothing -- so not knowing the zone
// breaks nothing: it is only why the time went back two hours after a
// power-off.
// ---------------------------------------------------------------------------

bool clock_utc_offset_known(void);
// The standard time of the zone in use, daylight saving left out.
int clock_utc_offset_minutes(void);

// Daylight saving time, kept as its own thing.
//
// The zones in the list are fixed offsets and must stay that way: Rome is +1,
// full stop, as on any map. DST is not another zone, it is one hour on top of
// the chosen one, and it has to be switched on and off twice a year without
// hunting for one's city on a different row of the list.
//
// So the two are kept apart: the list says where, this switch says when. What
// reaches the system is their sum.
//
// With the zone located by the network, clock_dst_enabled() says whether its
// rule has daylight saving on right now, and clock_set_dst() only changes the
// hand-set value, used again once the clock is manual.
bool clock_dst_enabled(void);
void clock_set_dst(bool on);

// Written by the time zone list, and can be forced from the configuration:
//   [clock]
//   utc_offset_minutes = 120
void clock_set_utc_offset_minutes(int minutes);

// True while the zone comes from the network: the clock is automatic and an
// attempt has located the device at least once. The zone list and the daylight
// saving switch are then the network's, not the user's.
bool clock_zone_automatic(void);

// The located zone's IANA name ("Europe/Rome"), or "" while
// clock_zone_automatic() is false.
const char *clock_zone_name(void);

// Puts the system clock back where it belongs; returns true if it had to move
// it.
//
// Why this exists: CLOCK_REALTIME does not advance while the SoC is suspended
// to RAM, and this device suspends whenever it is left alone. Every suspend
// therefore loses wall-clock time -- "three minutes behind after twenty" is a
// handful of them adding up, not a crystal drifting. Two independent ways back
// to the truth, tried in order:
//
//   1. the RTC, which runs on its own power island and is written through by
//      clock_set(); and
//   2. CLOCK_BOOTTIME, which (unlike CLOCK_MONOTONIC) counts the time spent
//      suspended, so the elapsed real time can be reconstructed from the last
//      known-good pair even with no usable RTC.
//
// Call it after every resume, at every screen wake, and on a slow timer.
bool clock_resync(void);

// Writes the current time through to the RTC, the way the stock player does on
// its way out (decompile: "hwclock -w --utc" in its shutdown path). Call it
// before powering off or rebooting -- without it everything the clock learned
// since it was last set is lost at power-off.
void clock_shutdown(void);

// ---------------------------------------------------------------------------
// How the time is written
//
// One setting for the whole device: the status bar, the screensaver and the
// rollers on the Date and time page. On by default -- 24-hour is the format the
// rest of the interface is written in, and whoever wants AM/PM says so.
// ---------------------------------------------------------------------------

bool clock_use_24h(void);
void clock_set_use_24h(bool on); // stored in the configuration

// "14:05" or "2:05 PM", per the setting. `hour` is always the real hour, 0 to
// 23; the conversion happens here.
void clock_format_hm(char *out, size_t out_size, int hour, int minute);

// The same in two parts, for a clock that draws AM and PM apart from the
// figures: "2:05" and "PM", or "14:05" and "".
void clock_format_hm_parts(char *time_out, size_t time_size, char *suffix_out, size_t suffix_size, int hour, int minute);

#endif /* CLOCK_H */
