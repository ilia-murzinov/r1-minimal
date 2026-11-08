#include "topbar.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/clocklabel.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/usb.h"
#include "src/system/audio/usbdac.h"

// Where the volume glyph changes: silent, quiet, loud.
#define VOLUME_LOW_PERCENT 1
#define VOLUME_HIGH_PERCENT 55

// A radio that is on but connected to nothing is drawn at this opacity; full
// opacity means something is on the other end.
#define RADIO_IDLE_OPA LV_OPA_40

// The radios get their own timer rather than riding the battery poll: five
// seconds is far too long to notice a network dropping, and reading two
// mutex-guarded structs costs nothing.
#define RADIO_POLL_MS 2000

// The jacks, the USB-C port and the charger. Fast enough that the glyph changes
// while the plug is still going in, which is the only rate that reads as
// "immediately".
#define JACK_POLL_MS 250

// How many of those polls have to agree before the charge state is acted on.
#define CHARGE_SETTLE_POLLS 3

// How far down a press has to end, with no drag having started, for the pull on
// the status bar to count as a flick and open the control centre anyway.
// Comfortably more than the wobble of a tap.
// Over the player's artwork the bar blurs whatever is drawn under it, as it is
// drawn, and darkens it towards black about as far as the block behind the
// player's controls (BACKDROP_BRIGHTNESS_PCT in cover.c), so the white type
// reads on any picture.
#define OVER_ART_BLUR_RADIUS 24
#define OVER_ART_DIM_OPA 148

static lv_obj_t *top_bar;
static lv_obj_t *bat_label;
static lv_obj_t *charge_icon; // tiny zap while charging — no battery shell
static lv_obj_t *vol_label;
static lv_obj_t *vol_icon;
static lv_obj_t *hp_icon; // headphone jack indicator: hidden / theme / gold
static lv_obj_t *play_icon;		 // play/pause indicator: hidden when nothing is loaded
static lv_obj_t *library_icon;	 // Detect changes at work, or just done
static lv_timer_t *library_timer; // takes the check glyph away
static void refresh_play_icon(const device_state_t *state);
static lv_obj_t *clock_label;
static lv_obj_t *bt_icon;   // bluetooth, leftmost of the right-hand group
static lv_obj_t *home_icon; // tap → main menu (Minimal R1)
static lv_timer_t *bt_reconnect_timer; // watches long-press reconnect outcome
static bool bt_reconnect_watching;
static lv_obj_t *container_left;  // volume group (and the clock, when "left")
static lv_obj_t *container_right; // battery group (and the clock, when "right")
static lv_timer_t *battery_timer;
static lv_timer_t *jack_timer;

// The last battery reading painted. Kept here rather than inside the poll so a
// theme switch can throw it away: the poll only repaints when the reading has
// moved, and new colours are not a new reading.
static int last_battery_percent = -2;
static int last_battery_charging = -1;

// A page painted in a colour of its own -- an album in its sleeve's -- has the
// bar in that colour too, a shade darker as the theme's own bar is, with the
// glyphs and the type in the ink worked out for it. Only while that page is
// the one shown: `tint_page` is the page being shown, set as it is loaded.
//
// The glyphs and the labels wear two styles added after the theme's, empty
// until a tint is in force, so a glyph whose colour means something -- the
// gold jack, the codec on the Bluetooth glyph, the volume warning -- keeps it:
// those are set on the object itself and win over any style.
static lv_obj_t *tint_screen;
static lv_obj_t *tint_page;
static lv_color_t tint_bg;
static lv_color_t tint_ink;
static bool tint_active;
static bool tint_held; // topbar_tint_hold()
static lv_style_t tint_text_style;
static lv_style_t tint_icon_style;

// Up over the player's artwork (topbar_set_over_artwork): white on the
// picture, whatever the theme or the page underneath says.
static bool over_art;
static lv_timer_t *radio_timer;

// Parses the sysfs capacity string. Returns -1 when it is not a number: the
// host build has no battery and its reader returns "!!".
static int parse_percent(const char *text) {
	if (!text || text[0] < '0' || text[0] > '9')
		return -1;

	int value = atoi(text);
	if (value < 0)
		value = 0;
	if (value > 100)
		value = 100;
	return value;
}

// Percent always; a small zap only while charge is actually going in.
static void update_battery_indicator(int percent, bool charging) {
	if (!bat_label) {
		return;
	}
	if (percent < 0) {
		lv_label_set_text(bat_label, "--%");
	} else {
		lv_label_set_text_fmt(bat_label, "%d%%", percent);
	}
	if (charge_icon) {
		lv_obj_set_hidden(charge_icon, !charging);
	}
}

// Redraws the clock. Before the time has been set there is nothing truthful to
// show, so it reads as blank rather than as 1970.
void topbar_refresh_clock(void) { clocklabel_refresh(clock_label); }

static void remember_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// Correct the wall clock against the RTC, or the boot clock, before writing
	// it down, so the remembered value is right and the bar stops drifting
	// behind between wakes.
	if (clock_resync()) {
		topbar_refresh_clock();
	}
	clock_remember();
}

// Volume, as a number with the glyph that matches its level.
void topbar_refresh_volume(int percent) {
	if (!vol_label) {
		return;
	}

	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	lv_label_set_text_fmt(vol_label, "%d", percent);

	// Past this threshold the number turns red, warning that the next steps are
	// the loud ones. The volume pop-up uses the same threshold.
	if (percent > VOLUME_WARN_PERCENT) {
		lv_obj_set_style_text_color(vol_label, VOLUME_WARN_COLOR, 0);
	} else {
		lv_obj_remove_local_style_prop(vol_label, LV_STYLE_TEXT_COLOR, 0);
	}

	const lv_image_dsc_t *icon = &icon_volume_high;
	if (percent < VOLUME_LOW_PERCENT) {
		icon = &icon_volume_mute;
	} else if (percent < VOLUME_HIGH_PERCENT) {
		icon = &icon_volume_low;
	}
	lv_image_set_src(vol_icon, icon);
}

// Shows or hides the headphone glyph to match what is in the jacks. Four times
// a second, off jack_timer, so the glyph changes while the plug is still going
// in; everything below returns at once when nothing has moved.
static void refresh_headphone_icon(void) {
	static int last_state = -1;
	static int last_usb = -1;
	int state = headphone_jack_state();

	// The same poll is where line out finds out that its jack has gone: the
	// mode holds the output at a fixed level, and that level does not belong to
	// whatever is plugged in next.
	lineout_check_jack();

	// And the USB-C port, on the same beat: a DAC or a pair of USB-C
	// headphones appearing means playback has somewhere else to go.
	usbaudio_poll();

	int usb = usbaudio_active() ? 1 : 0;
	if (state == last_state && usb == last_usb) {
		return;
	}

	// Something left one of the sockets. Pausing belongs here and not further
	// down because the icon work below returns early in several places, and
	// because this is the only code that remembers what was plugged in a
	// quarter of a second ago.
	//
	// -1 is the first poll after boot, where there is no "before" to have left.
	// And only the output actually carrying the music counts: with a DAC on the
	// port or headphones on the radio, the jacks are not what anyone is
	// listening to, and pulling a cable out of an unused socket must not stop
	// the album.
	if (last_usb == 1 && usb == 0) {
		player_output_unplugged("the USB-C DAC");
	} else if (last_state > JACK_NONE && state == JACK_NONE && !usb) {
		char device[160];
		audio_get_output_device(device, sizeof(device));
		if (!device[0] || strcmp(device, "default") == 0) {
			player_output_unplugged(last_state == JACK_BALANCED ? "the 4.4 mm jack" : "the 3.5 mm jack");
		}
	}

	last_state = state;
	last_usb = usb;
	if (!hp_icon) {
		return;
	}

	// With a DAC on the port the sockets are not what the sound comes out of,
	// whatever is plugged into them.
	if (usb) {
		lv_image_set_src(hp_icon, &icon_usbaudioout);
		lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		lv_obj_set_hidden(hp_icon, false);
		return;
	}
	lv_image_set_src(hp_icon, &icon_headphones);

	if (state == JACK_NONE) {
		lv_obj_set_hidden(hp_icon, true);
		return;
	}

	lv_obj_set_hidden(hp_icon, false);
	// B&W minimal: no gold for balanced — same ink as SE.
	lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR, 0);
	lv_obj_remove_local_style_prop(hp_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
}

// Bluetooth left of the charge percentage. Always shown when the radio exists
// so it can be long-pressed (reconnect last device): off is the dimmest
// reading, on-but-idle is RADIO_IDLE_OPA, connected is full.
void topbar_refresh_radios(void) {
	if (!bt_icon) {
		return;
	}
	if (!bluetooth_available()) {
		lv_obj_set_hidden(bt_icon, true);
		return;
	}
	lv_obj_set_hidden(bt_icon, false);
	lv_opa_t opa = LV_OPA_20;
	if (bluetooth_get_enabled()) {
		opa = bluetooth_get_state() == BT_STATE_CONNECTED ? LV_OPA_COVER : RADIO_IDLE_OPA;
	}
	lv_obj_set_style_image_opa(bt_icon, opa, 0);
	// B&W minimal: no codec-family tint on the glyph.
	lv_obj_remove_local_style_prop(bt_icon, LV_STYLE_IMAGE_RECOLOR, 0);
	lv_obj_remove_local_style_prop(bt_icon, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
}

// How long the check glyph stays once a Detect changes run is over.
#define LIBRARY_CHECKED_MS 4000

static void library_timer_cb(lv_timer_t *timer) {
	(void)timer;
	library_timer = NULL; // a one-shot timer, deleted by LVGL after this call
	lv_obj_set_hidden(library_icon, true);
}

void topbar_set_library_check(topbar_library_t state) {
	if (!library_icon) {
		return;
	}
	if (library_timer) {
		lv_timer_delete(library_timer);
		library_timer = NULL;
	}
	switch (state) {
	case TOPBAR_LIBRARY_IDLE:
		lv_obj_set_hidden(library_icon, true);
		break;
	case TOPBAR_LIBRARY_CHECKING:
		lv_image_set_src(library_icon, &icon_library_checking);
		lv_obj_set_hidden(library_icon, false);
		break;
	case TOPBAR_LIBRARY_CHECKED:
		lv_image_set_src(library_icon, &icon_library_checked);
		lv_obj_set_hidden(library_icon, false);
		library_timer = lv_timer_create(library_timer_cb, LIBRARY_CHECKED_MS, NULL);
		lv_timer_set_repeat_count(library_timer, 1);
		break;
	}
}

static void radio_timer_cb(lv_timer_t *timer) {
	(void)timer;
	topbar_refresh_radios();
}

// The playback indicator: the play glyph while a track runs, the pause glyph
// while one is loaded but stopped, nothing at all when there is no track. It
// takes the same colour as everything else in the bar rather than the accent,
// which would make it shout.
void topbar_refresh_playback(void) {
	device_state_t state;
	device_state_get(&state);
	refresh_play_icon(&state);
}

static void refresh_play_icon(const device_state_t *state) {
	if (!play_icon) {
		return;
	}

	if (state->status == AUDIO_STATUS_PLAYING) {
		lv_image_set_src(play_icon, &icon_play_status);
	} else if (state->current_file[0]) {
		lv_image_set_src(play_icon, &icon_pause_status);
	} else {
		lv_obj_set_hidden(play_icon, true);
		return;
	}

	// Same ink as the rest of the bar (B&W minimal). Bluetooth still has its
	// own glyph; the play mark does not turn blue for wireless.
	lv_obj_remove_local_style_prop(play_icon, LV_STYLE_IMAGE_RECOLOR, 0);
	lv_obj_set_hidden(play_icon, false);
}

// The things noticed with the hands, on their own fast timer: what is in the
// jacks, what is on the USB-C port, and whether the charger is in. Everything
// here returns at once when nothing has changed.
static void jack_timer_cb(lv_timer_t *timer) {
	(void)timer;

	refresh_headphone_icon();

	// The charger, on the same beat and for the same reason as the jacks: it is
	// something the user just did with their hands, and up to five seconds of a
	// battery icon that has not noticed reads as a cable that has not gone in.
	// The reads are a handful of small sysfs files; the repaint below is what
	// costs, and it only happens when the reading has actually moved.
	device_state_refresh_battery();

	device_state_t state;
	device_state_get(&state);
	int percent = parse_percent(state.battery_percent);

	// "Charging" out of the power supplies means a cable that is supplying,
	// which stays true long after the battery has stopped taking anything. What
	// the bolt and the red LED are for is a charge in progress, so the two
	// endings of one are taken out of it: the battery full, and the charger held
	// off at the configured limit. The cable is still in and the player still
	// runs off it -- there is simply nothing left to indicate.
	bool done = state.battery_charging && percent >= 0 && (percent >= 100 || power_charging_held());

	// One number for the whole answer, so the two halves can never disagree:
	// 0 no cable, 1 a cable with nothing going into the battery, 2 charging.
	int charge_state = !state.battery_charging ? 0 : (done ? 1 : 2);

	// A new answer has to come back the same way CHARGE_SETTLE_POLLS times
	// before it is acted on.
	//
	// The supplies do not read straight for a few seconds after the panel comes
	// back: the panel's rails are opened and closed at the AXP2101, and
	// /sys/class/power_supply/usb is that same PMIC, so the load step at the
	// unblank lands in the middle of what is being read. Unfiltered it is a bolt
	// appearing and vanishing four times a second, and -- the reason this is
	// here -- an LED pattern rewritten on every flip. The charging red is a
	// ramp programmed into the SGM31324, and a rewrite restarts the ramp, so
	// what comes out is a red that stutters for as long as the reading wobbles.
	//
	// Three polls is three quarters of a second, which still shows a cable going
	// in while the plug is still moving.
	static int settled = -1; // the charge state currently being acted on
	static int candidate = -1;
	static int candidate_polls;

	if (charge_state != candidate) {
		candidate = charge_state;
		candidate_polls = 1;
		if (settled >= 0 && candidate != settled) {
			static const char *const NAMES[] = {"no cable", "cable, not charging", "charging"};
			printf("battery: the charge reading says %s; waiting for it to settle\n", NAMES[candidate]);
		}
	} else if (candidate_polls < CHARGE_SETTLE_POLLS) {
		candidate_polls++;
	}
	if (settled < 0 || candidate_polls >= CHARGE_SETTLE_POLLS) {
		settled = candidate;
	}

	if (percent == last_battery_percent && settled == last_battery_charging) {
		return;
	}
	last_battery_percent = percent;
	last_battery_charging = settled;

	update_battery_indicator(percent, settled == 2);

	// And the LED, which says the same thing in the dark, from the same settled
	// answer: the pattern is never rewritten for a reading that is about to
	// change back. The cable goes with it -- a charge that has finished still
	// leaves the player on a charger, and that is what keeps the LED off the
	// darken-in-standby option.
	led_set_charging(settled == 2);
	led_set_on_charger(settled != 0);
}

// The periodic poll: battery, volume persistence, playback glyph and the
// status LED all ride this one timer.
static void timer_update_cb(lv_timer_t *timer) {
	(void)timer;

	// Both levels are written down from this poll rather than on every step of a
	// held volume key, so a long press costs one config write instead of fifty.
	// Which key each one goes to is alsa-controls' business: it is what knows
	// which profile is current, and it writes nothing when neither has moved.
	//
	// Ungated by "remember volume": that setting is about what the player comes
	// back to after a restart (see main.c), not about whether the two profiles
	// keep their own levels while it runs.
	volume_profile_persist();

	device_state_t state;
	device_state_get(&state);

	topbar_refresh_volume((int)state.volume);
	refresh_play_icon(&state);

	// The battery and the charge LED ride jack_timer instead, which runs often
	// enough for plugging the charger in to show at once. What is on this poll
	// is what nobody is standing over -- the playback colour, Wi-Fi transfer,
	// DAC mode.
	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate,
						!state.live && audio_get_dsd_multiple() > 0);

	// And Bluetooth receiver mode, which like the transfer is a thing the
	// player is doing rather than something it is playing: the blue stays on
	// for as long as the mode is, screen off included.
	led_set_bt_receiver(btreceiver_is_active());

	// And DAC mode, which plays nothing through audio.c and so reads as a
	// stopped player above. Only while the cable is really there: the mode is
	// turned off from its own page and nowhere else, so a player unplugged and
	// pocketed would otherwise keep the LED lit against the standby setting.
	usbdac_state_t dac;
	usbdac_get_state(&dac);
	bool dac_live = dac.active && usb_vbus_present();
	led_set_dac(dac_live, dac_live && dac.streaming, dac.sample_rate);
}

// Where the clock sits in the bar: centred on its own, first in the volume group
// on the left, or last in the battery group on the right. Re-parenting is all it
// takes, since the two side groups are flex rows and their layout absorbs it.
void topbar_set_clock_position(int pos) {
	if (!clock_label) {
		return;
	}

	if (pos == TOPBAR_CLOCK_HIDDEN) {
		lv_obj_set_hidden(clock_label, true);
		return;
	}
	lv_obj_set_hidden(clock_label, false);

	if (pos == TOPBAR_CLOCK_LEFT) {
		lv_obj_set_parent(clock_label, container_left);
		lv_obj_move_to_index(clock_label, 0); // before the volume number
		lv_obj_set_align(clock_label, LV_ALIGN_DEFAULT);
	} else if (pos == TOPBAR_CLOCK_RIGHT) {
		lv_obj_set_parent(clock_label, container_right); // appended, so it lands right of the battery
		lv_obj_set_align(clock_label, LV_ALIGN_DEFAULT);
	} else {
		lv_obj_set_parent(clock_label, top_bar);
		lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
	}
}

// Re-runs the battery paint so its colours follow a theme switch. The reading
// is thrown away first, or the poll would see the same numbers as last time and
// keep the old colours.
//
// The radios come with it: the Bluetooth glyph carries a codec colour set by
// hand, which has a light and a dark variant and would otherwise keep the one
// from before the switch until the next poll.
static void topbar_refresh_theme(void) {
	last_battery_percent = -2;
	last_battery_charging = -1;
	if (jack_timer) {
		lv_timer_ready(jack_timer);
	}
	if (radio_timer) {
		lv_timer_ready(radio_timer);
	}
}

// Minimal R1: no control centre. The status bar still exists for volume and
// battery; a downward drag on it does nothing.
static void topbar_drag_cb(lv_event_t *e) { (void)e; }

static void home_icon_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_is_open()) {
		player_sheet_close(false);
	}
	if (lv_screen_active() == main_menu_screen) {
		return;
	}
	screen_history_reset();
	switch_screen_no_history(main_menu_screen);
}

bool topbar_bt_reconnect_pending(void) { return bt_reconnect_watching; }

static void bt_reconnect_stop_watch(void) {
	bt_reconnect_watching = false;
	if (bt_reconnect_timer) {
		lv_timer_pause(bt_reconnect_timer);
	}
}

static void bt_reconnect_cancel_cb(void) {
	bt_reconnect_stop_watch();
	bluetooth_reconnect_cancel();
	topbar_refresh_radios();
}

static void bt_reconnect_poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (!bt_reconnect_watching) {
		return;
	}
	char name[BT_NAME_MAX];
	bt_op_t op = bluetooth_take_op_result(name, sizeof(name));
	if (op != BT_OP_OK && op != BT_OP_FAILED) {
		return;
	}
	bt_reconnect_stop_watch();
	topbar_refresh_radios();

	if (op == BT_OP_OK) {
		if (name[0]) {
			char message[BT_NAME_MAX + 32];
			snprintf(message, sizeof(message), tr("bt_connected_to"), name);
			toast_success(message);
		} else {
			toast_success("bt_device_connected");
		}
		return;
	}
	gui_notify_popup("bt_connect_failed");
}

// Long-press: reconnect the last headphones. No tap action — Settings owns
// the on/off switch.
static void bt_icon_long_cb(lv_event_t *e) {
	(void)e;
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	if (!bluetooth_available()) {
		gui_notify_popup("bt_bluetooth_not_available");
		return;
	}

	char name[BT_NAME_MAX];
	if (!bluetooth_last_device(name, sizeof(name))) {
		toast_plain("bt_no_last_device");
		return;
	}
	if (!bluetooth_reconnect_last()) {
		toast_plain("bt_no_last_device");
		return;
	}

	char message[BT_NAME_MAX + 40];
	snprintf(message, sizeof(message), tr("bt_connecting_to"), name);
	toast_busy_cancel(message, bt_reconnect_cancel_cb);
	bt_reconnect_watching = true;
	if (bt_reconnect_timer) {
		lv_timer_resume(bt_reconnect_timer);
		lv_timer_ready(bt_reconnect_timer);
	}
	topbar_refresh_radios();
}

// Nothing inside the bar takes a press; the bar does.
//
// Every press on the status bar has one meaning -- pull the control centre down
// -- and topbar_drag_cb() is on the bar itself, so a child that takes the press
// first is a strip of bar where the gesture does not start. LVGL hands the
// press to the topmost clickable object under the finger and nothing bubbles up
// unless asked to, so one clickable child is one dead patch.
//
// It is easy to add one by accident: lv_obj_create() returns a clickable object
// by default, while lv_image_create() and lv_label_create() do not. So this is
// done by walking what was built rather than by remembering at each call --
// the battery indicator is two plain objects and had been taking every press
// that landed on it.
static void topbar_clear_child_presses(lv_obj_t *obj) {
	uint32_t n = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
		lv_obj_set_clickable(child, false);
		topbar_clear_child_presses(child);
	}
}

void topbar_init(gui_config_t *cfg) {
	if (top_bar != NULL) {
		return;
	}

	// The bar lives on the top layer so it stays above every screen.
	top_bar = lv_obj_create(lv_layer_top());
	lv_obj_set_size(top_bar, cfg->screen_width, cfg->top_bar_height);
	lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_add_style(top_bar, &theme_style_panel, 0);
	lv_obj_set_style_border_width(top_bar, 0, 0);
	lv_obj_set_style_radius(top_bar, 0, 0);
	lv_obj_set_scrollable(top_bar, false);
	lv_obj_set_clickable(top_bar, true);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(top_bar, topbar_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The default container padding would inset the bar's contents, so it goes.
	lv_obj_set_style_pad_left(top_bar, 0, 0);
	lv_obj_set_style_pad_right(top_bar, 0, 0);
	lv_obj_set_style_pad_top(top_bar, 0, 0);
	lv_obj_set_style_pad_bottom(top_bar, 0, 0);

	// The left-hand group: volume, then the jack and playback indicators, laid
	// out as a flex row so the two sides of the bar balance around the clock.
	container_left = lv_obj_create(top_bar);
	lv_obj_set_size(container_left, cfg->screen_width / 2, cfg->top_bar_height);
	lv_obj_align(container_left, LV_ALIGN_LEFT_MID, cfg->padding, 0);
	lv_obj_set_style_bg_opa(container_left, 0, 0);
	lv_obj_set_style_border_width(container_left, 0, 0);
	lv_obj_set_style_radius(container_left, 0, 0);
	lv_obj_set_style_pad_all(container_left, 0, 0);
	lv_obj_set_style_pad_gap(container_left, 8, 0);
	lv_obj_set_scrollable(container_left, false);
	// Not clickable, so presses reach the bar itself, whose drag handler pulls
	// the control panel down.
	lv_obj_set_clickable(container_left, false);
	lv_obj_set_flex_flow(container_left, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(container_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Glyph first, then the number: the icon says what the number means.
	vol_icon = lv_image_create(container_left);
	lv_image_set_src(vol_icon, &icon_volume_high);
	lv_obj_add_style(vol_icon, &theme_style_icon, 0);

	vol_label = lv_label_create(container_left);
	lv_label_set_text(vol_label, "--");
	lv_obj_add_style(vol_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(vol_label, &font_ui_24, 0);

	// The jack indicator: appears when headphones are plugged in, theme-coloured
	// for the 3.5 mm jack and gold for the 4.4 mm balanced one.
	hp_icon = lv_image_create(container_left);
	lv_image_set_src(hp_icon, &icon_headphones);
	lv_obj_add_style(hp_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(hp_icon, true);

	// What the player is doing, right of the jack indicator: play while a track
	// runs, pause while one is loaded and stopped, hidden when nothing is loaded.
	play_icon = lv_image_create(container_left);
	lv_image_set_src(play_icon, &icon_play_status);
	lv_obj_add_style(play_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(play_icon, true);

	// Right of it, Detect changes while it runs and briefly once it is done
	// (topbar_set_library_check).
	library_icon = lv_image_create(container_left);
	lv_image_set_src(library_icon, &icon_library_checking);
	lv_obj_add_style(library_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(library_icon, true);

	// The right-hand group: radios, charge percentage and battery, packed to the
	// right edge.
	container_right = lv_obj_create(top_bar);
	lv_obj_set_size(container_right, cfg->screen_width, cfg->top_bar_height);
	lv_obj_align(container_right, LV_ALIGN_RIGHT_MID, -cfg->padding, 0);
	lv_obj_set_style_bg_opa(container_right, 0, 0);
	lv_obj_set_style_border_width(container_right, 0, 0);
	lv_obj_set_style_radius(container_right, 0, 0);
	// The container's default padding would push the battery about ten pixels
	// further in than the page padding everything else lines up with.
	lv_obj_set_style_pad_all(container_right, 0, 0);
	lv_obj_set_style_pad_gap(container_right, 8, 0);
	lv_obj_set_scrollable(container_right, false);
	lv_obj_set_clickable(container_right, false);
	lv_obj_set_flex_flow(container_right, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(container_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Bluetooth first (always shown when the radio exists — long-press reconnect),
	// then charge % and battery shell.
	bt_icon = lv_image_create(container_right);
	lv_image_set_src(bt_icon, &icon_bluetooth_status);
	lv_obj_add_style(bt_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(bt_icon, true);

	// Percent, then a tiny zap only while charge is going in (no battery shell).
	bat_label = lv_label_create(container_right);
	lv_label_set_text(bat_label, "--%");
	topbar_set_battery_percent(config_get_int("screen", "battery_percent", 1) != 0);
	lv_obj_add_style(bat_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(bat_label, &font_ui_24, 0);

	charge_icon = lv_image_create(container_right);
	lv_image_set_src(charge_icon, &icon_zap);
	lv_image_set_scale(charge_icon, (256 * 18) / 34); // 34px zap → ~18px
	lv_obj_add_style(charge_icon, &theme_style_icon, 0);
	lv_obj_set_hidden(charge_icon, true);

	// Clock object kept (tint / refresh paths) but Minimal R1 keeps it hidden.
	clock_label = clocklabel_create(top_bar, &font_ui_24, 24, &font_ui_14, 14);
	lv_obj_add_style(clock_label, &theme_style_text, 0);
	lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
	topbar_set_clock_position(TOPBAR_CLOCK_HIDDEN);

	// The tint's two styles, after the theme's so that they win over it; empty
	// until a tinted page is shown (topbar_tint_on_screen).
	lv_style_init(&tint_text_style);
	lv_style_init(&tint_icon_style);
	lv_obj_t *tinted_icons[] = {vol_icon, hp_icon, play_icon, library_icon, bt_icon, charge_icon};
	for (size_t i = 0; i < sizeof(tinted_icons) / sizeof(tinted_icons[0]); i++) {
		lv_obj_add_style(tinted_icons[i], &tint_icon_style, 0);
	}
	lv_obj_t *tinted_labels[] = {vol_label, bat_label, clock_label};
	for (size_t i = 0; i < sizeof(tinted_labels) / sizeof(tinted_labels[0]); i++) {
		lv_obj_add_style(tinted_labels[i], &tint_text_style, 0);
	}

	// The current time is written to the config, and checked against the RTC, so
	// a device whose RTC does not hold comes back up roughly right instead of at
	// 1970, and never further behind than one period.
	//
	// Every five minutes, and paused with the screen off: each round rewrites
	// device_config.ini on UBIFS, for a value that only matters if the battery
	// is pulled abruptly. What counts is writing the time when the screen goes
	// off, which power.c does by calling clock_remember() there (plus
	// clock_shutdown() on exit). The timer only earns its keep while somebody is
	// holding the device.
	lv_timer_t *remember_timer = lv_timer_create(remember_timer_cb, 300000, NULL);
	power_pause_in_standby(remember_timer);

	// What is left on the slow poll: the two volume levels written down, the
	// playback glyph, and the LED's playback colour. Nobody stands over any of
	// them with a cable in their hand.
	battery_timer = lv_timer_create(timer_update_cb, 5000, NULL);
	lv_timer_ready(battery_timer); // run immediately on startup

	// The jacks, the USB-C port and the charger keep their own, much faster
	// beat: on the five-second battery poll, plugging something in would take
	// up to five seconds to show, long enough to look broken to somebody still
	// holding the plug. A few small sysfs reads at this rate are cheap, and the
	// repainting only happens when what they find has changed.
	jack_timer = lv_timer_create(jack_timer_cb, JACK_POLL_MS, NULL);
	lv_timer_ready(jack_timer);
	// Slowed rather than stopped in standby: a jack pulled out with the screen
	// dark still has to take line out down with it, the LED still has to follow
	// the charger, and both icons have to be right before the panel comes back.
	power_slow_in_standby(jack_timer, 2000);
	// Slowed in standby, never stopped: this poll also tells led.c whether the
	// charger is in and re-evaluates the off-in-standby countdown. Stopped, the
	// LED would stay aqua for ever and never go red on a charger.
	power_slow_in_standby(battery_timer, 5000);

	// The radios keep their own faster beat: a network coming or going should
	// show up in a couple of seconds, not five.
	radio_timer = lv_timer_create(radio_timer_cb, RADIO_POLL_MS, NULL);
	lv_timer_ready(radio_timer);
	power_pause_in_standby(radio_timer);

	// Last, once everything that lives on the bar exists: see the note over the
	// function. The bar keeps its own CLICKABLE, which is what the drag hangs
	// off; only what is inside it gives presses up.
	topbar_clear_child_presses(top_bar);

	// Home: centred where the clock was. Added after clear_child_presses so it
	// stays clickable (everything else on the bar deliberately is not).
	home_icon = lv_image_create(top_bar);
	lv_image_set_src(home_icon, &icon_home);
	lv_obj_add_style(home_icon, &theme_style_icon, 0);
	lv_obj_add_style(home_icon, &tint_icon_style, 0);
	lv_obj_align(home_icon, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_clickable(home_icon, true);
	lv_obj_set_ext_click_area(home_icon, 12);
	lv_obj_add_event_cb(home_icon, home_icon_cb, LV_EVENT_CLICKED, NULL);

	// Same for Bluetooth: clear_child_presses just walked it, so clickability
	// is put back here (needed for LONG_PRESSED). Minimal R1 has no
	// control-centre drag on the bar, so a hittable glyph is fine.
	lv_obj_set_clickable(bt_icon, true);
	lv_obj_set_ext_click_area(bt_icon, 12);
	lv_obj_add_event_cb(bt_icon, bt_icon_long_cb, LV_EVENT_LONG_PRESSED, NULL);

	bt_reconnect_timer = lv_timer_create(bt_reconnect_poll_cb, 250, NULL);
	lv_timer_pause(bt_reconnect_timer);
	power_pause_in_standby(bt_reconnect_timer);

	// The battery shell is recoloured by hand rather than by a style, so it needs
	// a repaint when the palette changes.
	theme_register_refresh(topbar_refresh_theme);
}

bool topbar_is_hidden(void) { return top_bar && lv_obj_is_hidden(top_bar); }

void topbar_set_battery_percent(bool shown) {
	if (!bat_label) {
		return;
	}
	if (shown) {
		lv_obj_set_hidden(bat_label, false);
	} else {
		lv_obj_set_hidden(bat_label, true);
	}
}

// Puts the bar in the tint or back in the theme, for the page now shown.
static void tint_apply(void) {
	if (!top_bar || over_art) {
		return; // the artwork's look holds for as long as the player is up
	}
	bool active = tint_screen && tint_page == tint_screen && !tint_held;
	if (active == tint_active && !active) {
		return;
	}
	tint_active = active;

	if (active) {
		lv_obj_set_style_bg_color(top_bar, tint_bg, 0);
		lv_style_set_text_color(&tint_text_style, tint_ink);
		lv_style_set_image_recolor(&tint_icon_style, tint_ink);
		lv_style_set_image_recolor_opa(&tint_icon_style, LV_OPA_COVER);
	} else {
		lv_obj_remove_local_style_prop(top_bar, LV_STYLE_BG_COLOR, 0);
		lv_style_remove_prop(&tint_text_style, LV_STYLE_TEXT_COLOR);
		lv_style_remove_prop(&tint_icon_style, LV_STYLE_IMAGE_RECOLOR);
		lv_style_remove_prop(&tint_icon_style, LV_STYLE_IMAGE_RECOLOR_OPA);
	}
	lv_obj_report_style_change(&tint_text_style);
	lv_obj_report_style_change(&tint_icon_style);

	last_battery_percent = -2; // next poll repaints percent / zap
}

// The blur and the dimming behind the bar, or neither.
static void over_art_backdrop(bool blur) {
	if (blur) {
		lv_obj_set_style_bg_color(top_bar, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(top_bar, OVER_ART_DIM_OPA, 0);
		lv_obj_set_style_blur_radius(top_bar, OVER_ART_BLUR_RADIUS, 0);
		lv_obj_set_style_blur_backdrop(top_bar, true, 0);
		lv_obj_set_style_blur_quality(top_bar, LV_BLUR_QUALITY_SPEED, 0);
	} else {
		lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
		lv_obj_remove_local_style_prop(top_bar, LV_STYLE_BLUR_RADIUS, 0);
		lv_obj_remove_local_style_prop(top_bar, LV_STYLE_BLUR_BACKDROP, 0);
		lv_obj_remove_local_style_prop(top_bar, LV_STYLE_BLUR_QUALITY, 0);
	}
}

void topbar_set_over_artwork(bool on, bool blur) {
	if (!top_bar) {
		return;
	}
	if (on) {
		over_art = true;
		over_art_backdrop(blur);
		lv_style_set_text_color(&tint_text_style, lv_color_white());
		lv_style_set_image_recolor(&tint_icon_style, lv_color_white());
		lv_style_set_image_recolor_opa(&tint_icon_style, LV_OPA_COVER);
		lv_obj_report_style_change(&tint_text_style);
		lv_obj_report_style_change(&tint_icon_style);
	} else if (over_art) {
		over_art = false;
		over_art_backdrop(false);
		lv_obj_remove_local_style_prop(top_bar, LV_STYLE_BG_OPA, 0);
		// Whatever the page underneath wants, from scratch: the tint's styles
		// hold the artwork's white until this puts them back.
		tint_active = true;
		tint_apply();
	} else {
		return;
	}
	last_battery_percent = -2;
}

void topbar_tint_on_screen(lv_obj_t *screen, bool on, lv_color_t bg, lv_color_t ink) {
	if (on) {
		tint_screen = screen;
		tint_bg = bg;
		tint_ink = ink;
		tint_active = false; // apply again even if it was on: the colours are new
	} else if (tint_screen == screen) {
		tint_screen = NULL;
	}
	if (!tint_page) {
		tint_page = lv_screen_active();
	}
	tint_apply();
}

void topbar_page_shown(lv_obj_t *screen) {
	tint_page = screen;
	tint_apply();
}

void topbar_tint_hold(bool held) {
	if (tint_held == held) {
		return;
	}
	tint_held = held;
	tint_apply();
}

void topbar_bring_to_front(void) {
	if (top_bar) {
		lv_obj_move_foreground(top_bar);
	}
}

// Shows or hides the whole bar. The player page hides it so the album art can
// start at the very top of the screen.
void topbar_set_hidden(bool hidden) {
	if (!top_bar) {
		return;
	}

	if (hidden) {
		lv_obj_set_hidden(top_bar, true);
	} else {
		lv_obj_set_hidden(top_bar, false);
	}
}
