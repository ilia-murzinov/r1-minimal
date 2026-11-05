#include "player.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/library/medialist.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/device/power.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/readahead.h"
#include "src/system/playback/device_state.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/lyrics.h"
#include "src/system/device/led.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/utils.h"
#include "src/system/audio/waveform.h"

#include "lvgl/src/core/lv_obj_event_private.h"
#include "lvgl/lvgl.h"


lv_obj_t *player_screen;

static lv_obj_t *player_menu;
static lv_obj_t *play_btn;
static lv_obj_t *play_btn_icon;
static lv_obj_t *song_title_label;
static lv_obj_t *song_artist_label;
static lv_obj_t *format_label; // "16/44.1 FLAC", right of the artist line
static lv_obj_t *fav_btn_obj;  // the button it sits on
static lv_obj_t *controls_row; // transport row: where the ellipsis lives, and the star in Studio
static lv_obj_t *fav_btn_icon; // the star above it

static void update_fav_button(void);
static void update_format_label(const device_state_t *state);
static void update_repeat_button(void);

// The colour the chrome on this page is drawn in: the theme's accent normally,
// and the sleeve's own while the alternative layout is up. Everything else on
// that layout takes its colour from the record -- the pills, the star's disc,
// the shape of the track -- and a blue play button in the middle of it was the
// one thing that still belonged to some other page.
static lv_color_t chrome_accent(void);
static lv_obj_t *progress_slider;
static lv_obj_t *elapsed_label; // start of the track, under the left end of the bar
static lv_obj_t *remaining_label; // end of the track, under the right end
// Where this track sits in the queue, between the two clocks: the row under the
// bar had an empty middle and the question -- how much of this is left -- is the
// same one the clocks answer.
static lv_obj_t *queue_position_label;
static lv_obj_t *repeat_btn_icon;
static lv_obj_t *prev_icon; // static so the over-cover recolor can reach them
static lv_obj_t *next_icon;
static lv_timer_t *progress_slider_timer;

// Runs only while a sleep timer is counting, which is the only time there is
// anything to ask. Paused otherwise, so an option nobody switched on does not
// wake the main loop once a second all night.
static lv_timer_t *sleep_timer;

// Where playback was when the poll last asked, and when that was. The tick is
// lv_tick_get()'s, which is the same clock the timer is driven by.
static lv_timer_t *smooth_timer;
static bool sheet_open; // the player panel is the page on show

// "Always show the status bar" (Settings > Appearance): the bar stays up over
// the player too. Over the picture that runs to the top edge -- Standard and
// Waveform -- it blurs and dims what lies under it as it is drawn, like the
// block behind the controls; Studio, whose whole background is that blurred
// picture already, moves down under it instead, the chevron and the heading
// with it (back_btn_player_below_bar).
static bool bar_always;
static int bar_height; // the status bar's, from the config
static void bar_over_player_update(void);
static double progress_anchor_secs;
static uint32_t progress_anchor_tick;
static bool progress_running;

// The smoothing runs only while there is a bar moving on a sheet that can be
// seen: sixteen wake-ups a second for a panel that is closed or dark are pure
// drain. Asked again by every poll, which is how it comes back after a wake,
// and at every opening and closing of the sheet.
static void smooth_timer_sync(void) {
	if (!smooth_timer) {
		return;
	}
	if (progress_running && sheet_open && power_screen_is_on()) {
		lv_timer_resume(smooth_timer);
	} else {
		lv_timer_pause(smooth_timer);
	}
}

// How often the UI reconciles itself against the playback thread. It polls
// slowly rather than not at all while stopped: the audio thread can report
// STOPPED for a moment in the middle of a track change, and a UI that stopped
// polling on that reading would stay frozen until the next touch.
#define POLL_PERIOD_PLAYING_MS 500
#define POLL_PERIOD_IDLE_MS 2000

// How far past their 56 px the buttons at the two ends of the transport row
// take a tap. Upwards and downwards the 84 px row still clips it; sideways the
// previous and next buttons start about 50 px further in, so 20 reaches
// neither.
#define CORNER_BTN_EXT_CLICK 20

// How often the progress display is carried forward between polls.
//
// The poll asks the playback thread where it is, and half a second is as often
// as that is worth asking. But half a second is also how long the bar and the
// shape of the track stood still before jumping, and a bar that moves twice a
// second reads as a bar that is broken.
//
// So the position between polls is worked out from the clock instead of being
// asked for -- playback runs in real time, so the arithmetic is exact -- and
// the drawing happens only when it would actually differ by a pixel. On a
// four-minute track that is about twice a second for each of the two, out of
// sixteen chances; the rest of the ticks cost a subtraction and a comparison.
#define SMOOTH_PERIOD_MS 60

// How many fast polls to keep running after playback stops, so the queue gets
// its chance to start the next track before the timer relaxes.
#define POLLS_FAST_AFTER_STOP 4

static int polls_since_stop = POLLS_FAST_AFTER_STOP;

// Height the controls block needs for its two lines of text, the bar, the
// clocks and the buttons. Only used as a floor on screens too short to fit a
// full-width square cover on top of it.
#define PLAYER_MENU_MIN_HEIGHT 210

// The controls block as the R3 Pro II has it, 720 - 480. A taller panel (the
// R1's 800) spreads what it has over the rows rather than leaving it empty
// above and below them: a fifth to each end and a fifth to each of the three
// gaps between the four rows.
#define PLAYER_MENU_REF_HEIGHT 240
#define PLAYER_MENU_PAD_VER 10
#define PLAYER_MENU_GAP 12

// Studio keeps only the bar, the clocks and the transport in the controls
// block, pushed to the bottom, with the gaps between them the standard
// arrangement has; the panel with the sleeve takes the rest of the screen
// above them. This is their height at PLAYER_MENU_GAP; a taller panel adds
// what its wider gaps take.
#define STUDIO_CONTROLS_H 172

// Album art: an image on top of a placeholder panel. The panel is always
// there, so the layout doesn't jump between a track that has a cover and one
// that doesn't -- the image is simply hidden in the latter case.
static lv_obj_t *cover_panel;
static lv_obj_t *cover_img;
static lv_obj_t *cover_placeholder_icon;

// The transport and overflow buttons, kept so the layout code can reach them.
static lv_obj_t *prev_btn_obj;
static lv_obj_t *next_btn_obj;
static lv_obj_t *more_btn_obj;
static lv_obj_t *more_btn_icon;
static lv_obj_t *repeat_btn_obj;
static lv_obj_t *below_slider_obj; // the two clocks under the bar
static cover_image_t current_cover;	   // pixels currently referenced by cover_img
static cover_image_t current_backdrop; // upside-down blurred copy behind the controls
static int cover_box_w, cover_box_h;   // the artwork spans the full screen width

// ---------------------------------------------------------------------------
// The two arrangements
//
// Standard is sleeve on top, and under it a block with the title, the star, the
// format, a bar and the transport.
//
// Alternative moves the track's own things onto the sleeve -- the title and
// artist in a pill in one bottom corner, the star in a disc in the other -- and
// turns the progress bar into the shape of the track. Everything it draws takes
// its colour from the sleeve rather than from the theme, which is why it only
// looks like anything at all over artwork.
//
// One set of widgets, moved between parents, and not two pages: the title, the
// star and the transport all have behaviour behind them, and a second copy of
// each would be a second place for that behaviour to be wrong.
// ---------------------------------------------------------------------------

// The progress bar: a thin track, and a knob that is a white ring around a
// smaller circle of the accent colour. The ring is a wide white border over an
// accent-coloured body rather than a second object, so it costs nothing and
// follows the knob on its own.
#define PROGRESS_TRACK_HEIGHT 10 // as thick as every other slider
#define PROGRESS_KNOB_GROW 9	 // how far the knob grows past the track
#define PROGRESS_KNOB_RING 5

// How the shape of the track is drawn, and how much room it gets. The bars are
// wide and well apart with rounded ends rather than a comb of hairlines: at
// this size a hairline is one pixel of a colour that is half background, and
// the whole thing reads as noise.
#define WAVE_HEIGHT 64
#define WAVE_BAR_GAP 3
#define WAVE_MIN_BAR 3	  // a silent column is still a mark, not a hole
#define WAVE_PAST_OPA 255 // the part already played
#define WAVE_TODO_OPA 80  // and the part still to come

// How far the pills and the disc sit in from the corner of the sleeve, and how
// big the disc is.
#define ALT_PAD 16
#define ALT_FAV_SIZE 64
#define ALT_PILL_PAD_H 18 // what a pill keeps to the left and right of its text
#define ALT_PILL_PAD_V 8
// How far past its own edges a transport button takes a press in this
// arrangement: into the gaps between them. The star takes the whole of its disc
// instead (apply_layout()).
#define ALT_EXT_CLICK 14
#define FAV_BTN_W 48
#define FAV_BTN_H 44

// What the sleeve's colour becomes once it has to be painted rather than
// thrown.
//
// album_tone is stretched to full strength (cover_tone_stretched), the way
// cover_dominant_tone() answers for Cover Flow, where it is a light behind the
// record and a light that is not strong is not seen. Poured into a solid pill the same number is a poster
// colour -- which is the complaint. So the hue is kept and everything else is
// pinned: the surface keeps a third of its distance from grey and sits at a
// fixed low brightness, the ink on it is always the same white, and the mark
// the waveform draws keeps rather more colour and a fixed high brightness so it
// still reads over a dark blurred sleeve. Two roles, one hue -- every album
// then looks like the same player rather than like a different skin.
#define ALT_SURFACE_SAT 34	// per cent of the distance from grey kept
#define ALT_SURFACE_LUMA 58 // and where it sits on 0..255
#define ALT_MARK_SAT 72
#define ALT_MARK_LUMA 196

// The setting, and whether it is in force right now.
//
// They are two different things because the alternative arrangement is built
// around a file on the card: the shape of the track comes from decoding it, the
// colour from its sleeve, and the pill in the corner from tags. So the setting
// says what the user wants for their music, and `layout_alt_now` says whether
// what is playing is music of that kind. Everything that draws asks the second.
static player_layout_t layout_choice; // the setting, saved in the config
static bool layout_alt_now;		// and whether the current source can use it
static bool layout_studio_now;	// likewise for the third arrangement
// The shape of the track and the sleeve's colour: up with the alternative
// layout, and kept when the words are shown over it.
static bool layout_wave_now;

static uint32_t album_tone; // the sleeve's colour, 0 when there is no sleeve
// The setting that has the Standard and Studio arrangements wear the sleeve's
// colour as their accent too, as Waveform always does.
static bool cover_accent;

static lv_obj_t *song_text_obj; // the title and artist, wherever they live now
static lv_obj_t *song_side_obj; // the star and the format, likewise
static void align_title_with_star(void);
static lv_obj_t *alt_text_col;	// the two pills, stacked at the foot of the sleeve
static lv_obj_t *alt_title_pill;
static lv_obj_t *alt_artist_pill;
static lv_obj_t *alt_fav_circle;
static lv_obj_t *wave_box;
static lv_obj_t *wave_canvas;
static uint8_t *wave_buf;
static int wave_w; // the buffer's width, which is what the painting must use
static uint8_t wave_bars[WAVEFORM_BARS];
// How many columns of this track's shape are known, from the left: all of them
// once it has been worked out, fewer while the worker is still reading the
// file, and the rest are drawn flat until they arrive.
static int wave_ready;
static int wave_drawn = -1; // where the playhead was, in pixels, when it was drawn
static uint32_t wave_drawn_tone = 1;
static int backdrop_w, backdrop_h;	   // size of the controls block it sits behind
// Studio shows the same blurred copy behind the whole screen, so there it is
// asked for at that shape instead: a picture made for the block behind the
// controls is a third of the height and stretching it up is both distorted and
// coarse. `backdrop_is_studio` is the shape the picture on hand was made at.
static int backdrop_studio_h;
static bool backdrop_is_studio;

static double current_total_length = 0; // cached from the last device_state snapshot, so slider math works between polls
static char progress_label_text[32];

// The length of the file the progress bar and its two clocks stand for. 0 while
// it is not known.
static double view_length;

// True while the controls sit on a loaded cover's dark backdrop. The play
// button wears its white disc there (it has to stand out against artwork);
// on the plain no-cover panel it is flat, like prev and next.
static bool chrome_over_cover;

// Which sleep timer, if any, owns what is playing.
//
// The Bluetooth receiver is none: it pushes its sound out through the external
// path and there is no pause to give it -- stopping the receiver is silence in
// a room where somebody else's phone is still playing. Everything else the
// transport plays is music and gets the music timer.
static sleeptimer_kind_t sleeptimer_kind_playing(void) {
	if (btreceiver_is_active()) {
		return SLEEPTIMER_COUNT;
	}
	return SLEEPTIMER_MUSIC;
}

// Applies a playback status to the play/pause button and the progress timer.
// Called both optimistically (right after a button press, before the audio
// thread has caught up) and from update_progress() every poll, which is what
// keeps the UI from going stale if the track stops on its own.
static void apply_playback_status(audio_status_t status) {
	bool playing = (status == AUDIO_STATUS_PLAYING);

	// The glyph is the accent colour, always: play and pause are the same
	// control in two states. Only the disc under it changes (see
	// set_over_cover): white over artwork and in the dark theme, dark in the
	// light theme when there is no cover to stand out against.
	//
	lv_image_set_src(play_btn_icon, playing ? &icon_pause : &icon_play);
	lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
	lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);

	// Keep polling either way, just less often when nothing is playing. The
	// timer must never be paused here: a track change is briefly reported as
	// STOPPED, and a timer paused on that reading leaves nothing running to
	// correct the progress bar and the play/pause icon.
	//
	// Dropping straight to the slow period on STOPPED is wrong too: the end of
	// a track is reported as STOPPED and the queue only advances on the next
	// poll, so the gap between two tracks would become the slow period, seconds
	// of silence reading as a player that stopped instead of going on. Stay on
	// the fast period for a moment after playback stops, which is exactly the
	// window the handover needs.
	if (playing) {
		polls_since_stop = 0;
	} else if (polls_since_stop < POLLS_FAST_AFTER_STOP) {
		polls_since_stop++;
	}

	bool settling = polls_since_stop < POLLS_FAST_AFTER_STOP;
	lv_timer_set_period(progress_slider_timer, (playing || settling) ? POLL_PERIOD_PLAYING_MS : POLL_PERIOD_IDLE_MS);
	lv_timer_resume(progress_slider_timer);

	// The status bar shows the same state, and told here rather than left to
	// wait for its own five-second poll.
	topbar_refresh_playback();

	// Every route into and out of playing comes through here -- the button, the
	// side keys, the end of a track, a remote, the poll putting the screen back
	// in step with the engine -- so it is the one place the sleep timers can be
	// told without being told twice. It is also the one place that knows what
	// is playing well enough to say which of the three it belongs to.
	sleeptimer_note_playing(playing ? sleeptimer_kind_playing() : SLEEPTIMER_COUNT);
	if (sleep_timer) {
		if (sleeptimer_any_running()) {
			lv_timer_resume(sleep_timer);
		} else {
			lv_timer_pause(sleep_timer);
		}
	}
}

// "3/24": which track of the queue is playing.
//
// It reads the queue itself, so it says the same thing whatever filled it.
//
// The number is the track's place in the LIST, not in the dealt order: under
// shuffle those are two different things, and the list is the one that means
// something. Counting the deal gives a number that climbs by one at every
// track, which reads as a position in an album but is not one, since the
// tracks are picked at random from all over the card. Outside shuffle the two
// orders are the same array and the reading is identical.
//
// Blank rather than "1/1" when there is nothing to be lost in: a single track.
bool player_queue_position(int *position, int *count) {
	*position = 0;
	*count = 0;
	int total = playlist_count();
	int index = playlist_current_index();
	if (total <= 1 || index < 0) {
		return false;
	}
	*position = (int)playlist_current_entry() + 1;
	*count = total;
	return true;
}

static void update_queue_position(void) {
	if (!queue_position_label) {
		return;
	}

	int count = playlist_count();
	int index = playlist_current_index();
	bool worth_saying = count > 1 && index >= 0;

	if (!worth_saying) {
		lv_label_set_text(queue_position_label, "");
		return;
	}
	lv_label_set_text_fmt(queue_position_label, "%d/%d", (int)playlist_current_entry() + 1, count);
}

// Where `position` sits along the bar, 0 to 1000, the slider's range.
static int view_value(double position) {
	if (view_length <= 0) {
		return 0;
	}
	double fraction = position / view_length;
	if (fraction < 0) {
		fraction = 0;
	}
	if (fraction > 1) {
		fraction = 1;
	}
	return (int)(fraction * 1000);
}

// The other way round: the second of the file that slider value `value` is.
static double view_seconds(int value) { return view_length * value / 1000.0; }

// Writes the two clocks that sit under the ends of the progress bar: elapsed
// on the left, total length on the right.
static void set_progress_label(double current_secs, double total_secs) {
	view_length = total_secs > 0 ? total_secs : 0;
	double shown = 0;
	if (view_length > 0) {
		lv_slider_set_value(progress_slider, view_value(current_secs), LV_ANIM_OFF);
		shown = current_secs;
		if (shown < 0) {
			shown = 0;
		}
	}

	formatDoubleSeconds(shown, progress_label_text, sizeof(progress_label_text));
	lv_label_set_text(elapsed_label, progress_label_text);

	formatDoubleSeconds(view_length, progress_label_text, sizeof(progress_label_text));
	lv_label_set_text(remaining_label, progress_label_text);

	// Written from here because this is the one place that runs on every poll
	// and on every track change, which is exactly when the answer can move.
	update_queue_position();
}

// True while a decode is in flight at the worker.
static bool cover_request_outstanding;

// What the artwork on screen IS, as cover_source_id() reports it, so the next
// track can be asked "the same picture?" before anything is thrown away. 0 when
// it is not known -- the answer has not come back yet, or there is no artwork.
static uint64_t cover_shown_id;

// The id of the picture now being decoded, when it was already known before the
// decode was asked for. Saves hashing the same file twice.
static uint64_t cover_incoming_id;

// Hashing never happens on this thread: it reads the picture out of the file,
// which is the work the loader exists to keep off the interface. So there is a
// question outstanding, and two reasons to have asked it.
static enum {
	COVER_ID_IDLE,
	COVER_ID_NEW_TRACK, // is the next track's picture the one already up?
	COVER_ID_MEASURE,   // what is the picture that just went up?
} cover_id_state;

// How often the loader's answers are looked for while one is outstanding. The
// progress poll comes round only every half second (two seconds when paused),
// and a track change asks twice -- what the picture is, then the picture -- so
// waiting for it added up to a second, or four, on top of the work itself.
// This timer runs only while a question is out and stops itself after.
#define COVER_POLL_MS 30
static lv_timer_t *cover_poll_timer;
static void cover_poll_start(void);
static void prefetch_next_cover(void);

// With artwork loaded the controls sit on the always-dark blurred backdrop,
// so the chrome -- titles, clocks, transport glyphs, the chevron -- must be
// light in both themes. Local style props are laid over the theme styles
// while the cover is up, and removed again when it goes, so the plain
// no-cover panel keeps following the theme.
// The picture at the width of the screen, and the note that stands in for it.
//
// One place, because Studio hides both -- the sleeve there is a smaller picture
// of its own over the blurred copy -- and the artwork arrives on a worker, so
// whatever the arrangement did when it was set up would be undone the moment a
// cover landed.
static void cover_show(bool have_cover) {
	if (!cover_img) {
		return;
	}
	bool studio = layout_studio_now;
	if (have_cover && !studio) {
		lv_obj_set_hidden(cover_img, false);
	} else {
		lv_obj_set_hidden(cover_img, true);
	}
	if (!cover_placeholder_icon) {
		return;
	}
	if (!have_cover && !studio) {
		lv_obj_set_hidden(cover_placeholder_icon, false);
	} else {
		lv_obj_set_hidden(cover_placeholder_icon, true);
	}
}

static void set_over_cover(bool on) {
	chrome_over_cover = on;

	lv_obj_t *const text_objs[] = {song_title_label, elapsed_label, remaining_label};
	lv_obj_t *const dim_objs[] = {song_artist_label, format_label, queue_position_label};
	lv_obj_t *const icon_objs[] = {prev_icon, next_icon};

	for (size_t i = 0; i < sizeof(text_objs) / sizeof(text_objs[0]); i++) {
		if (!text_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_text_color(text_objs[i], lv_color_white(), 0);
		} else {
			lv_obj_remove_local_style_prop(text_objs[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
	for (size_t i = 0; i < sizeof(dim_objs) / sizeof(dim_objs[0]); i++) {
		if (!dim_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_text_color(dim_objs[i], lv_color_make(200, 200, 200), 0);
		} else {
			lv_obj_remove_local_style_prop(dim_objs[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
	for (size_t i = 0; i < sizeof(icon_objs) / sizeof(icon_objs[0]); i++) {
		if (!icon_objs[i]) {
			continue;
		}
		if (on) {
			lv_obj_set_style_image_recolor(icon_objs[i], lv_color_white(), 0);
			lv_obj_set_style_image_recolor_opa(icon_objs[i], LV_OPA_COVER, 0);
		} else {
			lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR, 0);
			lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR_OPA, 0);
		}
	}

	// The disc: white over artwork and in the dark theme, where white reads
	// as the brightest thing on screen. The one case it has to go dark is the
	// light theme with no cover -- a white disc on a near-white panel is
	// invisible.
	if (play_btn) {
		bool white_disc = on || theme()->dark;
		lv_obj_set_style_bg_color(play_btn, white_disc ? lv_color_white() : theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	}
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}

	// The floating chevron parks over the artwork's top-left corner.
	back_btn_over_cover(on);
}

// Told before the loaded track's artwork is freed. The screensaver borrows
// those pixels and can still be up behind a blanked panel when a track
// changes, so it has to let go first.
static void (*cover_release_cb)(void);

void player_set_cover_release_cb(void (*cb)(void)) { cover_release_cb = cb; }

void player_cover_box(int *w, int *h) {
	if (w) {
		*w = cover_box_w;
	}
	if (h) {
		*h = cover_box_h;
	}
}

bool player_cover_copy(int size, cover_image_t *out, uint64_t *id) {
	*id = 0;
	// Only a settled cover: while one is coming, or a track change is still
	// asking whether its picture is the same, what is up may be the last
	// track's.
	if (!current_cover.pixels || cover_request_outstanding || cover_id_state != COVER_ID_IDLE) {
		return false;
	}
	if (!cover_scaled_copy(&current_cover, size, out)) {
		return false;
	}
	*id = cover_shown_id;
	return true;
}

// The path whose artwork is currently up, so that asking for the same one again
// is not work on the UI thread for nothing.
static char cover_shown_path[512];

static void reload_cover(const char *filepath);

// Shows the placeholder and asks the worker for the album art of `filepath`.
// Nothing is read or decoded here: on the X1600E that work is seconds long,
// and doing it on this thread at every track change -- including the automatic
// one when a song ends -- froze whatever the user was doing at that moment,
// the file browser most of all. The finished pictures are collected by
// apply_cover_result(), from cover_poll_cb() and the progress timer tick.
// Rec. 601 luma, which is what "how bright does this look" means to an eye and
// not to a sum of three channels.
static int luma_of(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }

// The sleeve's colour with its strength cut to `sat` per cent of the distance
// from grey and its brightness moved to `want`. Darkening is a plain ratio, so
// no channel can clip; brightening walks each channel the same fraction of the
// way to white, which keeps the hue where it was.
static lv_color_t tone_shaped(int sat, int want) {
	uint32_t raw = album_tone;
	if (!raw) {
		lv_color_t accent = theme()->accent;
		raw = ((uint32_t)accent.red << 16) | ((uint32_t)accent.green << 8) | accent.blue;
	}
	int r = (int)((raw >> 16) & 0xFF), g = (int)((raw >> 8) & 0xFF), b = (int)(raw & 0xFF);

	int grey = luma_of(r, g, b);
	r = grey + (r - grey) * sat / 100;
	g = grey + (g - grey) * sat / 100;
	b = grey + (b - grey) * sat / 100;

	int luma = luma_of(r, g, b);
	if (luma > want && luma > 0) {
		r = r * want / luma;
		g = g * want / luma;
		b = b * want / luma;
	} else if (luma < want && luma < 255) {
		int step = (want - luma) * 255 / (255 - luma);
		r += (255 - r) * step / 255;
		g += (255 - g) * step / 255;
		b += (255 - b) * step / 255;
	}
	return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// What the pills and the disc are filled with, and what the waveform is drawn
// in. Cover Flow throws the same hue under the record in the middle, which is
// what makes the two pages look like they belong to the same player.
static lv_color_t alt_surface(void) { return tone_shaped(ALT_SURFACE_SAT, ALT_SURFACE_LUMA); }
static lv_color_t alt_mark(void) { return tone_shaped(ALT_MARK_SAT, ALT_MARK_LUMA); }

// The accent the transport, the progress bar and the sung line wear: the
// sleeve's colour in the Waveform arrangement, and in the other two as well
// when the setting asks for it and there is a sleeve to take it from. The same
// shade in all three, light enough to read over the dark backdrop every
// arrangement puts behind a sleeve.
static lv_color_t chrome_accent(void) {
	// Minimal R1: black chrome on the white play disc (and other transport).
	// theme()->accent is white for B&W UI — that made the glyph vanish.
	return lv_color_black();
}

// The type on that surface. Worked out rather than fixed so that a change to
// ALT_SURFACE_LUMA cannot quietly leave the title unreadable.
static lv_color_t alt_ink(void) {
	lv_color_t s = alt_surface();
	return luma_of(s.red, s.green, s.blue) > 140 ? lv_color_hex(0x1A1A1A) : lv_color_white();
}

static void lyrics_repaint_lit(void);

// The two glyphs that wear the accent, repainted whenever the accent of the
// moment can have moved: the play/pause disc and the repeat/shuffle mode.
static void paint_transport_tint(void) {
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, chrome_accent(), 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
	if (repeat_btn_icon) {
		update_repeat_button();
	}
	// The progress bar, which Waveform hides under the shape of the track.
	if (progress_slider) {
		lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	}
	lyrics_repaint_lit(); // the sung line wears it too
}

// Hides the artist's pill when there is no artist. An empty pill is a blank
// lozenge sitting on the sleeve for no reason.
static void alt_pills_sync(void) {
	if (!alt_artist_pill || !song_artist_label) {
		return;
	}
	const char *artist = lv_label_get_text(song_artist_label);
	if (artist && artist[0]) {
		lv_obj_set_hidden(alt_artist_pill, false);
	} else {
		lv_obj_set_hidden(alt_artist_pill, true);
	}
}

// Puts the sleeve's colour on everything the alternative layout tints -- and,
// in the standard layout, takes it off again. That second half is the point:
// these are local style properties on widgets the two arrangements share, so
// leaving them behind painted the standard page in the last sleeve's ink.
static void paint_alt_tint(void) {
	if (!alt_title_pill) {
		return;
	}

	if (!layout_alt_now) {
		if (song_artist_label) {
			lv_obj_remove_local_style_prop(song_artist_label, LV_STYLE_TEXT_OPA, 0);
		}
		// And the title and the artist go back to being whatever the page they
		// are on says they are. Asked of the one function that knows -- the
		// colour depends on whether there is artwork behind them, not on the
		// theme alone, and a second copy of that rule here would be a second
		// place for it to go out of date.
		set_over_cover(chrome_over_cover);
		update_fav_button(); // which owns the star's colour in both arrangements
		paint_transport_tint();
		wave_drawn = -1;
		return;
	}

	lv_color_t surface = alt_surface();
	lv_color_t ink = alt_ink();

	lv_obj_set_style_bg_color(alt_title_pill, surface, 0);
	lv_obj_set_style_bg_color(alt_artist_pill, surface, 0);
	lv_obj_set_style_bg_color(alt_fav_circle, surface, 0);

	if (song_title_label) {
		lv_obj_set_style_text_color(song_title_label, ink, 0);
	}
	if (song_artist_label) {
		lv_obj_set_style_text_color(song_artist_label, ink, 0);
		lv_obj_set_style_text_opa(song_artist_label, LV_OPA_80, 0);
	}
	update_fav_button();
	alt_pills_sync();
	paint_transport_tint();

	wave_drawn = -1; // the bars are the old colour
}

// Whether what is playing is an ordinary file on the card.
//
// Asked of the state rather than of a flag raised at the start: a flag has to
// be cleared on every route by which the track can change, and the first route
// forgotten leaves the player dressed as the wrong thing.
static bool playing_local_file(const device_state_t *state) { return state->current_file[0] != '\0'; }

// Puts the progress slider over the waveform, or back where it belongs.
//
// The slider is the only thing on the page that knows how to turn a finger into
// a seek -- the waveform is a picture and nothing more -- so the alternative
// layout must not hide it. It moves inside the waveform's box instead,
// stretched over the whole of it so that a press anywhere on the bars is a
// press on the slider, with every part of it drawn as nothing. What is seen is
// the waveform; what is dragged is the slider.
// Leaves the shape of the track, where the panel above it overlaps it, to the
// slider laid over it.
static void cover_panel_hit_test_cb(lv_event_t *e) {
	lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
	if (!info || !wave_box || lv_obj_is_hidden(wave_box)) {
		return;
	}
	lv_area_t wave;
	lv_obj_get_coords(wave_box, &wave);
	const lv_point_t *p = info->point;
	if (p->x >= wave.x1 && p->x <= wave.x2 && p->y >= wave.y1 && p->y <= wave.y2) {
		info->res = false;
	}
}

static void slider_over_waveform(bool over) {
	if (!progress_slider) {
		return;
	}

	if (over && wave_box) {
		lv_obj_set_parent(progress_slider, wave_box);
		lv_obj_set_size(progress_slider, lv_pct(100), WAVE_HEIGHT);
		lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(progress_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
		lv_obj_set_style_opa(progress_slider, LV_OPA_TRANSP, LV_PART_KNOB);
		lv_obj_move_foreground(progress_slider); // over the canvas, which takes no presses anyway
		return;
	}

	if (lv_obj_get_parent(progress_slider) == wave_box) {
		lv_obj_set_parent(progress_slider, player_menu);
		lv_obj_move_to_index(progress_slider, lv_obj_get_index(wave_box) + 1);
	}
	lv_obj_set_size(progress_slider, lv_pct(100), PROGRESS_TRACK_HEIGHT);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_BG_OPA, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_BG_OPA, LV_PART_INDICATOR);
	lv_obj_remove_local_style_prop(progress_slider, LV_STYLE_OPA, LV_PART_KNOB);
}

// ---------------------------------------------------------------------------
// Studio: the sleeve in the middle, the blurred artwork behind the whole screen
//
// Nothing here is drawn by hand. The sleeve is the picture the page already
// decoded, shown at the size this arrangement wants, and the background is the
// blurred copy the page already makes for behind the controls, asked for at the
// size of the whole screen instead. What this section owns is where they go and
// when.
// ---------------------------------------------------------------------------

#define STUDIO_MARGIN 14	  // what the sleeve keeps to the side edges
#define STUDIO_HEAD_H 78	  // the title, the artist and the ellipsis
#define STUDIO_COVER_GAP 24	  // between the head and the top of the sleeve
#define STUDIO_HEAD_GAP 6	  // between the head and what lies under it
#define STUDIO_QUALITY_GAP 10 // between the sleeve and the line under it
#define STUDIO_QUALITY_H 30
#define STUDIO_BOTTOM 10 // under that line, before the controls begin
#define STUDIO_COVER_MAX_PCT 83

static lv_obj_t *studio_bg;		  // the blurred sleeve, the size of the screen
static lv_obj_t *studio_box;	  // what everything else is laid out on
static lv_obj_t *studio_head;	  // title and artist across the top
static lv_obj_t *studio_text_col; // the two of them, stacked and centred
static lv_obj_t *studio_cover;
static lv_obj_t *studio_empty;		  // the square shown where a track has no artwork
static lv_obj_t *studio_empty_icon;
static lv_obj_t *studio_quality;	  // the icon and the format line under the sleeve
static lv_obj_t *studio_quality_icon;
static bool studio_up;
static int studio_box_w, studio_box_h; // the panel this arrangement is laid out on
static int menu_pad_ver = PLAYER_MENU_PAD_VER; // the controls block's spacing, see PLAYER_MENU_REF_HEIGHT
static int menu_gap = PLAYER_MENU_GAP;
static int studio_cover_size;

// ---------------------------------------------------------------------------
// Lyrics
//
// For a file on the card, a page of words beside the sleeve: a finger
// drawing the artwork from right to left pulls the words in as the sleeve
// goes out, and drawing them back from left to right brings the sleeve back.
// The bar and the buttons stay exactly where they are. Everything else turns
// into Studio's look as the words come in: the blurred sleeve behind the whole
// screen, and the title and artist at the top. That blur is made in the
// background as soon as the artwork arrives, so it is there before the first
// swipe; Studio already has it, and there the words only take the sleeve's
// place.
//
// Timed lyrics follow the music, the line being sung lit in the accent colour
// and held in the middle of the column, the words moving up under it one line
// at a time; a finger scrolling them takes over for a few seconds, and a tap
// on a line plays from there. The column fades out at its top and bottom
// edges the way a scrolling title does at its ends.
//
// The swipe is there on every file, words or not. On one without, it asks
// whether to look for them on LRCLIB (lrclib.h); what is found is listed to
// pick from (lyricsearch.h), the one picked is saved beside the file, and the
// page comes in with it.
// ---------------------------------------------------------------------------

#define LYRICS_FADE_PX 56
#define LYRICS_LINE_GAP 16
#define LYRICS_DIM_OPA LV_OPA_40
#define LYRICS_USER_HOLD_MS 4000
#define LYRICS_STEP_MS 320 // one line moving up into the middle
#define LYRICS_SLIDE_MS 220
// The page comes or goes once the finger has carried it this fraction of the
// way; short of it, it goes back.
#define LYRICS_COMMIT_FRACTION 4
#define LYRICS_DRAG_PX 10 // sideways past this, a swipe and not a tap
// Behind the words on a track with no artwork: dark in either theme, since
// the words are white.
#define LYRICS_FULL 1000 // lyrics_progress with the page all the way in

static bool lyrics_wanted;  // pulled in by a swipe
static bool lyrics_allowed; // wanted, and a file on the card playing
static bool lyrics_now;		// on screen: allowed, and the file has words
static lv_obj_t *lyrics_pane; // the page: the column and the note
static lv_obj_t *lyrics_head; // the title and artist at the top while the words are up
static lv_obj_t *lyrics_head_col;
static lv_obj_t *lyrics_head_title;
static lv_obj_t *lyrics_head_artist;
static cover_image_t lyrics_screen_pic; // the blurred sleeve at the shape of the screen
static unsigned lyrics_bg_generation;
static int32_t lyrics_progress; // 0 the sleeve, LYRICS_FULL the words
// A line tapped: where it plays from, and until when the column waits for the
// player to get there rather than follow the position from before the jump.
static int32_t lyrics_tap_ms = -1;
static uint32_t lyrics_tap_until;
#define LYRICS_TAP_WAIT_MS 2000
static int lyrics_pane_x;		// where the page rests
static lv_obj_t *lyrics_view;
static lv_obj_t *lyrics_note; // "no lyrics", in the middle of the column
static lyrics_t lyrics_cur;
static bool lyrics_loaded;		   // lyrics_cur belongs to lyrics_path, found or not
static char lyrics_path[1024];	   // the file the view is for
static unsigned lyrics_generation; // stale answers from the loader are dropped
static int lyrics_lit = -1;		   // the line drawn as being sung
static uint32_t lyrics_user_until; // no following until then: a finger is reading
static bool lyrics_centred;		   // the lit line is where it belongs
static lv_image_dsc_t lyrics_mask;
static uint8_t *lyrics_mask_pixels;
static int lyrics_view_h;

// Timed lines have room above the first and below the last, so either can be
// brought to the middle; untimed ones read from the top.
static void lyrics_pad(void) {
	int around = lyrics_cur.synced ? lyrics_view_h / 2 - 20 : LYRICS_FADE_PX / 2;
	lv_obj_set_style_pad_top(lyrics_view, around, 0);
	lv_obj_set_style_pad_bottom(lyrics_view, lyrics_cur.synced ? around : LYRICS_FADE_PX, 0);
}

// An A8 mask the size of the column, opaque but for a ramp at the top and at
// the bottom. Built again only when the size changes.
static void lyrics_mask_build(int w, int h) {
	if (lyrics_mask_pixels && (int)lyrics_mask.header.w == w && (int)lyrics_mask.header.h == h) {
		return;
	}
	if (lyrics_view) {
		lv_obj_set_style_bitmap_mask_src(lyrics_view, NULL, 0);
	}
	free(lyrics_mask_pixels);
	lyrics_mask_pixels = NULL;
	if (w <= 0 || h <= 2 * LYRICS_FADE_PX) {
		return;
	}
	lyrics_mask_pixels = malloc((size_t)w * (size_t)h);
	if (!lyrics_mask_pixels) {
		return;
	}
	for (int y = 0; y < h; y++) {
		int edge = y < h - 1 - y ? y : h - 1 - y;
		uint8_t a = edge >= LYRICS_FADE_PX ? 255 : (uint8_t)(edge * 255 / LYRICS_FADE_PX);
		memset(lyrics_mask_pixels + (size_t)y * (size_t)w, a, (size_t)w);
	}
	memset(&lyrics_mask, 0, sizeof(lyrics_mask));
	lyrics_mask.header.magic = LV_IMAGE_HEADER_MAGIC;
	lyrics_mask.header.cf = LV_COLOR_FORMAT_A8;
	lyrics_mask.header.w = (uint32_t)w;
	lyrics_mask.header.h = (uint32_t)h;
	lyrics_mask.header.stride = (uint32_t)w;
	lyrics_mask.data = lyrics_mask_pixels;
	lyrics_mask.data_size = (uint32_t)((size_t)w * (size_t)h);
	lv_obj_set_style_bitmap_mask_src(lyrics_view, &lyrics_mask, 0);
}

static void lyrics_note_show(const char *tag) {
	if (!lyrics_note) {
		return;
	}
	if (tag) {
		lv_label_set_text(lyrics_note, tr(tag));
		lv_obj_set_hidden(lyrics_note, false);
	} else {
		lv_obj_set_hidden(lyrics_note, true);
	}
}

// The column's scroll position, for its step animation.
static void lyrics_scroll_exec(void *obj, int32_t y) { lv_obj_scroll_to_y(obj, y, LV_ANIM_OFF); }

// The column's lines, one label each, from lyrics_cur.
static void lyrics_fill(void) {
	if (!lyrics_view) {
		return;
	}
	lv_obj_clean(lyrics_view);
	lyrics_pad();
	lyrics_lit = -1;
	lyrics_user_until = 0;
	lyrics_centred = false;
	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	lv_obj_scroll_to_y(lyrics_view, 0, LV_ANIM_OFF);

	if (!lyrics_loaded) {
		lyrics_note_show(NULL); // still being read
		return;
	}
	if (lyrics_cur.count == 0) {
		lyrics_note_show("lyrics_none");
		return;
	}
	lyrics_note_show(NULL);

	for (int i = 0; i < lyrics_cur.count; i++) {
		lv_obj_t *line = lv_label_create(lyrics_view);
		lv_label_set_text(line, lyrics_cur.lines[i].text[0] ? lyrics_cur.lines[i].text : " ");
		lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(line, lv_pct(100));
		lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(line, &font_ui_26, 0);
		lv_obj_set_style_text_opa(line, lyrics_cur.synced ? LYRICS_DIM_OPA : LV_OPA_COVER, 0);
		lv_obj_set_event_bubble(line, true);
		lv_obj_set_clickable(line, false);
	}
}

typedef struct {
	unsigned generation;
	char path[1024];
	char title[256];
	lyrics_t lyrics;
	bool found;
} lyrics_job_t;

static void lyrics_settle(void);

static void lyrics_arrived(void *user) {
	lyrics_job_t *job = user;
	if (job->generation == lyrics_generation && strcmp(job->path, lyrics_path) == 0) {
		lyrics_free(&lyrics_cur);
		lyrics_cur = job->lyrics; // handed over, found or empty
		lyrics_loaded = true;
		lyrics_fill();
		lyrics_settle();
	} else {
		lyrics_free(&job->lyrics);
	}
	free(job);
}

static void *lyrics_worker(void *arg) {
	lyrics_job_t *job = arg;
	job->found = lyrics_load(job->path, job->title, &job->lyrics);
	if (!gui_post(lyrics_arrived, job)) {
		lyrics_free(&job->lyrics);
		free(job);
	}
	return NULL;
}

// Only a file on the card has words to show.
static bool lyrics_available(void) {
	device_state_t state;
	device_state_get(&state);
	return playing_local_file(&state);
}

// The page is up, or on its way in or out under a finger.
static bool lyrics_on_screen(void) { return lyrics_pane && !lv_obj_is_hidden(lyrics_pane); }

// Words being read are not an idle screen: while the lyrics are up on an open
// player, the idle timer does not blank the panel under them. Asked again
// wherever either half changes; the hold itself ignores a call that changes
// nothing.
static void lyrics_screen_hold_update(void) { power_hold_screen_for_view(sheet_open && lyrics_on_screen()); }

// The words of the file playing are known before any swipe: on a track with
// none the swipe asks to download them, and the page goes away by itself on
// one.
static bool lyrics_has_words(void) { return lyrics_loaded && lyrics_cur.count > 0; }

// The lyrics of the file playing, read on a thread of their own as soon as it
// starts: the tags of a FLAC are read whole, pictures included, and that is
// not the interface's to wait for. Asked again only when the file changes.
static void lyrics_request(const device_state_t *state) {
	if (!state->current_file[0] || !playing_local_file(state)) {
		return;
	}
	if (strcmp(state->current_file, lyrics_path) == 0) {
		return;
	}
	snprintf(lyrics_path, sizeof(lyrics_path), "%s", state->current_file);
	lyrics_free(&lyrics_cur);
	lyrics_loaded = false;
	lyrics_generation++;
	lyrics_fill();

	lyrics_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = lyrics_generation;
	snprintf(job->path, sizeof(job->path), "%s", state->current_file);
	snprintf(job->title, sizeof(job->title), "%s", state->metadata.title);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job);
	}
}

// Brings `label` to the middle of the column, always in the same short time
// whatever the distance, so the words step up like a typewriter's paper.
static void lyrics_centre(lv_obj_t *label) {
	lv_obj_update_layout(lyrics_view);
	lv_area_t line_area;
	lv_area_t view_area;
	lv_obj_get_coords(label, &line_area);
	lv_obj_get_coords(lyrics_view, &view_area);
	int32_t from = lv_obj_get_scroll_y(lyrics_view);
	int32_t to = from + (line_area.y1 + line_area.y2) / 2 - (view_area.y1 + view_area.y2) / 2;
	if (to < 0) {
		to = 0;
	}

	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	if (to == from) {
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_view);
	lv_anim_set_exec_cb(&a, lyrics_scroll_exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, LYRICS_STEP_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void lyrics_paint_line(int index, bool lit) {
	if (!lyrics_view || index < 0 || index >= (int)lv_obj_get_child_count(lyrics_view)) {
		return;
	}
	lv_obj_t *label = lv_obj_get_child(lyrics_view, index);
	// Unlit, the line wears the column's colour (see lyrics_look()).
	if (lit) {
		lv_obj_set_style_text_color(label, chrome_accent(), 0);
	} else {
		lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
	}
	lv_obj_set_style_text_opa(label, lit ? LV_OPA_COVER : LYRICS_DIM_OPA, 0);
}

static void lyrics_repaint_lit(void) { lyrics_paint_line(lyrics_lit, true); }

// The line being sung, lit and held in the middle of the column. Once a
// finger has scrolled the words, the column comes back to it a few seconds
// after the finger stops -- or, with the music paused, once it plays again:
// until then the words stay where the finger left them.
static void lyrics_follow(double seconds) {
	if (!lyrics_on_screen() || !lyrics_view || !lyrics_cur.synced || lyrics_cur.count == 0) {
		return;
	}
	int32_t ms = (int32_t)(seconds * 1000.0);
	if (lyrics_tap_ms >= 0) {
		// Until the player reports a position near the tapped line, the one
		// it reports is from before the jump: following it would send the
		// column back there and then forward again.
		if ((int32_t)(lv_tick_get() - lyrics_tap_until) < 0 && LV_ABS(ms - lyrics_tap_ms) > 1500) {
			return;
		}
		lyrics_tap_ms = -1;
	}
	int line = lyrics_line_at(&lyrics_cur, ms);
	if (line != lyrics_lit) {
		lyrics_paint_line(lyrics_lit, false);
		lyrics_lit = line;
		lyrics_paint_line(line, true);
		lyrics_centred = false;
	}

	if (lyrics_centred) {
		return;
	}
	if (lyrics_user_until &&
		((int32_t)(lv_tick_get() - lyrics_user_until) < 0 || audio_get_status() != AUDIO_STATUS_PLAYING)) {
		return;
	}
	lyrics_user_until = 0;
	lyrics_centred = true;
	if (line >= 0 && line < (int)lv_obj_get_child_count(lyrics_view)) {
		lyrics_centre(lv_obj_get_child(lyrics_view, line));
	} else {
		lyrics_centre(lv_obj_get_child(lyrics_view, 0));
	}
}

static void lyrics_scroll_cb(lv_event_t *e) {
	(void)e;
	// A finger, not the column following the music: that scrolls from a timer.
	if (lv_indev_active()) {
		lv_anim_delete(lyrics_view, lyrics_scroll_exec);
		lyrics_user_until = lv_tick_get() + LYRICS_USER_HOLD_MS;
		lyrics_centred = false;
	}
}

// The blurred sleeve the words sit on: Studio's own, or the one made for the
// other two arrangements. NULL while there is none.
static const lv_image_dsc_t *lyrics_backdrop(void) {
	if (layout_studio_now) {
		return current_backdrop.pixels ? &current_backdrop.dsc : NULL;
	}
	return lyrics_screen_pic.pixels ? &lyrics_screen_pic.dsc : NULL;
}

// Keeps the title and artist at the top saying what the row below says.
static void lyrics_head_sync(void) {
	if (!lyrics_head || !song_title_label || !song_artist_label) {
		return;
	}
	scrolltext_set(lyrics_head_title, lv_label_get_text(song_title_label));
	scrolltext_set(lyrics_head_artist, lv_label_get_text(song_artist_label));
}

// Studio's look for the arrangements that are not Studio: its blurred
// sleeve behind the whole screen, and the surfaces that would cover it --
// the sleeve's panel and the controls' own blurred block -- made clear. Put on
// in one step when the words start to come in, not faded with the finger: a
// screen-sized blend redrawn on every frame of the swipe is what this
// processor cannot keep up with. Only with a picture to show; without one the
// page keeps its own background, whole, and the words take the theme's
// colours.
static bool lyrics_look_on;

static void lyrics_look(bool on) {
	if (layout_studio_now) {
		on = false; // Studio is the look already
	}
	const lv_image_dsc_t *backdrop = lyrics_backdrop();
	if (on && !backdrop) {
		on = false;
	}
	if (on) {
		lv_image_set_src(studio_bg, backdrop);
		lv_obj_set_hidden(studio_bg, false);
		lv_obj_move_background(studio_bg);
		lv_obj_set_style_bg_opa(cover_panel, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(player_menu, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_image_opa(player_menu, LV_OPA_TRANSP, 0);
	} else if (lyrics_look_on && !layout_studio_now) {
		lv_obj_set_hidden(studio_bg, true);
		lv_obj_remove_local_style_prop(cover_panel, LV_STYLE_BG_OPA, 0);
		lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_OPA, 0);
		lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_IMAGE_OPA, 0);
	}
	lyrics_look_on = on;
	// The words are white over a blurred sleeve, and in the theme's own
	// colour over the page's own background where there is no picture.
	lv_color_t ink = lyrics_backdrop() ? lv_color_white() : theme()->text_primary;
	lv_obj_set_style_text_color(lyrics_view, ink, 0);
	lv_obj_set_style_text_color(lyrics_note, ink, 0);
	lv_obj_set_style_text_color(lyrics_head_title, ink, 0);
	lv_obj_set_style_text_color(lyrics_head_artist, lyrics_backdrop() ? lv_color_make(200, 200, 200) : theme()->text_secondary, 0);
}

// What travels with the sleeve: it goes out to the left as the words come in.
static void lyrics_shift(lv_obj_t *obj, int32_t x) {
	if (!obj) {
		return;
	}
	if (x == 0) {
		lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSLATE_X, 0);
	} else {
		lv_obj_set_style_translate_x(obj, x, 0);
	}
}

// The star of the arrangement with the shape of the track, which lives in a
// disc on the sleeve: while the words are up it moves to the head, at the
// right of the names, and back into its disc when they go.
static bool lyrics_star_up;

static void lyrics_star_to_head(bool up) {
	if (up == lyrics_star_up || !fav_btn_obj || !alt_fav_circle) {
		return;
	}
	lyrics_star_up = up;
	if (up) {
		lv_obj_set_parent(fav_btn_obj, lyrics_head);
		lv_obj_align(fav_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	} else {
		lv_obj_set_parent(fav_btn_obj, alt_fav_circle);
		lv_obj_center(fav_btn_obj);
	}
}

// Everything the swipe moves, at `progress` of the way from the sleeve (0)
// to the words (LYRICS_FULL). Only positions change from one frame to the
// next; at 0 every style touched is handed back, so the arrangement is
// exactly what it was before the first swipe.
static void lyrics_progress_apply(int32_t progress) {
	if (!lyrics_pane) {
		return;
	}
	if (progress < 0) {
		progress = 0;
	}
	if (progress > LYRICS_FULL) {
		progress = LYRICS_FULL;
	}
	lyrics_progress = progress;
	bool rest = progress == 0;
	int32_t travel = cover_box_w - lyrics_pane_x;

	if (rest) {
		lv_obj_set_hidden(lyrics_pane, true);
	} else if (lv_obj_is_hidden(lyrics_pane)) {
		lv_obj_set_hidden(lyrics_pane, false);
		lv_obj_move_foreground(lyrics_pane);
	}
	lyrics_screen_hold_update();
	lv_obj_set_x(lyrics_pane, lyrics_pane_x + travel * (LYRICS_FULL - progress) / LYRICS_FULL);
	if (rest || !lyrics_look_on) {
		lyrics_look(!rest);
	}

	int32_t away = -(int32_t)cover_box_w * progress / LYRICS_FULL;
	if (layout_studio_now) {
		lyrics_shift(studio_cover, away);
		lyrics_shift(studio_empty, away);
		lyrics_shift(studio_quality, away);
		return;
	}
	lyrics_shift(cover_img, away);
	lyrics_shift(cover_placeholder_icon, away);
	if (!layout_alt_now) {
		return; // the names stay in their row below the sleeve
	}

	// With the shape of the track the names are pills on the sleeve, so they
	// go with it, and come up at the top once the words are in.
	lyrics_shift(alt_text_col, away);
	lyrics_shift(alt_fav_circle, away);
	bool full = progress == LYRICS_FULL;
	lyrics_star_to_head(full);
	if (full) {
		lyrics_head_sync();
		lv_obj_set_hidden(lyrics_head, false);
		lv_obj_move_foreground(lyrics_head);
	} else {
		lv_obj_set_hidden(lyrics_head, true);
	}
}

// While the page moves, the fade at its edges is left off: it is drawn by
// rendering the column into a layer of its own and masking it, which is fine
// once a scroll step and far too much for every frame of a swipe.
static void lyrics_fade(bool on) {
	if (lyrics_view && lyrics_mask_pixels) {
		lv_obj_set_style_bitmap_mask_src(lyrics_view, on ? &lyrics_mask : NULL, 0);
	}
}

static void lyrics_progress_exec(void *obj, int32_t value) {
	(void)obj;
	lyrics_progress_apply(value);
}

static void lyrics_slid_cb(lv_anim_t *a) {
	(void)a;
	lyrics_fade(true);
}

static void lyrics_slide_to(int32_t progress) {
	lv_anim_delete(lyrics_pane, lyrics_progress_exec);
	if (lyrics_progress == progress) {
		lyrics_progress_apply(progress);
		lyrics_fade(true);
		return;
	}
	lyrics_fade(false);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_pane);
	lv_anim_set_exec_cb(&a, lyrics_progress_exec);
	lv_anim_set_values(&a, lyrics_progress, progress);
	lv_anim_set_duration(&a, LYRICS_SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, lyrics_slid_cb);
	lv_anim_start(&a);
}

// The head's place: Studio's, between the chevron and the right edge.
static int lyrics_head_top(void) {
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	return head_top < 2 ? 2 : head_top;
}

// Where the page goes in the arrangement on show: in Studio from under the
// head to above the controls, over the sleeve; with the shape of the track
// from under the head that takes the names; in the standard arrangement from
// under the chevron. Outside Studio the page covers the whole square.
static void lyrics_layout(void) {
	if (!lyrics_pane) {
		return;
	}
	int head_bottom = lyrics_head_top() + STUDIO_HEAD_H + STUDIO_HEAD_GAP;
	int top = layout_alt_now || layout_studio_now ? head_bottom : back_btn_centre_y() + 28;
	int pane_y = 0;
	int x = 0, w = cover_box_w, h = cover_box_h - top;
	if (layout_studio_now) {
		pane_y = top;
		int bottom = studio_box_h - STUDIO_BOTTOM;
		if (layout_wave_now && wave_canvas) {
			bottom -= WAVE_HEIGHT - PROGRESS_TRACK_HEIGHT;
		}
		x = STUDIO_MARGIN;
		w = studio_box_w - 2 * STUDIO_MARGIN;
		h = bottom - top;
	}
	lyrics_pane_x = x;
	int view_x = layout_studio_now ? 0 : STUDIO_MARGIN;
	int view_y = top - pane_y;
	int view_w = w - 2 * view_x;

	// Into the new arrangement with the styles of the old one handed back.
	lv_anim_delete(lyrics_pane, lyrics_progress_exec);
	lyrics_progress_apply(0);
	// apply_layout() has just put the star where this arrangement keeps it,
	// and the head belongs to the one before.
	lyrics_star_up = false;
	if (lyrics_head) {
		lv_obj_set_hidden(lyrics_head, true);
	}

	lv_obj_set_y(lyrics_pane, pane_y);
	lv_obj_set_size(lyrics_pane, w, h + view_y);
	lv_obj_set_pos(lyrics_view, view_x, view_y);
	lv_obj_set_size(lyrics_view, view_w, h);
	lyrics_view_h = h;
	lyrics_pad();
	lyrics_mask_build(view_w, h);
	lv_obj_set_size(lyrics_note, view_w, LV_SIZE_CONTENT);
	lv_obj_set_pos(lyrics_note, view_x, view_y + h / 2 - 20);

	if (lyrics_head) {
		lv_obj_set_pos(lyrics_head, STUDIO_MARGIN, lyrics_head_top());
		lv_obj_set_size(lyrics_head, cover_box_w - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);
	}
	lyrics_progress_apply(lyrics_now ? LYRICS_FULL : 0);
}

// Brings the page up, or puts it away, and remembers it either way: it stays
// up from one track to the next.
static void lyrics_commit(bool on) {
	lyrics_wanted = on;
	device_state_t state;
	device_state_get(&state);
	lyrics_allowed = on && playing_local_file(&state);
	lyrics_now = lyrics_allowed && lyrics_has_words();
	if (lyrics_now) {
		lyrics_slide_to(LYRICS_FULL);
		lyrics_request(&state);
		lyrics_follow(state.progress_current_secs);
	} else {
		lyrics_slide_to(0);
	}
}

// The words for the file playing have been read: a page wanted and up stays
// up if there are any and goes if there are none; one wanted and put away for
// a track without words comes back on the first track that has some.
static void lyrics_settle(void) {
	bool now = lyrics_allowed && lyrics_has_words();
	if (now == lyrics_now || !lyrics_pane) {
		return;
	}
	lyrics_now = now;
	lyrics_slide_to(now ? LYRICS_FULL : 0);
	if (now) {
		device_state_t state;
		device_state_get(&state);
		lyrics_follow(state.progress_current_secs);
	}
}

// The swipe that pulls the page in (on the sleeve, right to left) or pushes it
// out (on the words, left to right). `opening` is the user data. Sideways
// only: a movement that reads as vertical is the column being scrolled, and
// a swipe the other way on the sleeve is the player being put away.
static bool lyrics_swiped; // the release that ends a swipe is not a tap on a line

static void lyrics_swiped_clear_cb(void *unused) {
	(void)unused;
	lyrics_swiped = false;
}

static void lyrics_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev || !lyrics_pane) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		tracking = opening ? !lyrics_now && lyrics_available() && lyrics_loaded && lyrics_has_words() : lyrics_now;
		engaged = false;
		lv_indev_get_point(indev, &start);
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dx = p.x - start.x;
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (lv_indev_get_scroll_obj(indev)) {
				tracking = false; // the column is being scrolled
				return;
			}
			if (LV_ABS(dy) >= LYRICS_DRAG_PX && LV_ABS(dy) > LV_ABS(dx)) {
				tracking = false;
				return;
			}
			if (LV_ABS(dx) < LYRICS_DRAG_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			if ((opening && dx > 0) || (!opening && dx < 0)) {
				tracking = false; // the other way: not this gesture
				return;
			}
			engaged = true;
			lyrics_swiped = true;
			lv_anim_delete(lyrics_pane, lyrics_progress_exec);
			lyrics_fade(false);
			if (opening) {
				lyrics_progress_apply(1); // up, so the request below finds it on screen
				device_state_t state;
				device_state_get(&state);
				lyrics_request(&state);
				lyrics_follow(state.progress_current_secs);
			}
		}
		int32_t moved = (int32_t)dx * LYRICS_FULL / cover_box_w;
		lyrics_progress_apply(opening ? -moved : LYRICS_FULL - moved);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		bool far = LV_ABS(dx) > cover_box_w / LYRICS_COMMIT_FRACTION;
		lyrics_commit(opening ? far : !far);
		// Cleared after this gesture's CLICKED has been seen, as for the sheet.
		lv_async_call(lyrics_swiped_clear_cb, NULL);
	}
}

// A tap on a timed line plays from where it is sung.
static void lyrics_tap_cb(lv_event_t *e) {
	(void)e;
	if (lyrics_swiped || !lyrics_cur.synced || !lyrics_view) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	uint32_t count = lv_obj_get_child_count(lyrics_view);
	for (uint32_t i = 0; i < count && i < (uint32_t)lyrics_cur.count; i++) {
		lv_area_t area;
		lv_obj_get_coords(lv_obj_get_child(lyrics_view, i), &area);
		// The gap between two lines belongs to the one above it.
		if (p.y >= area.y1 && p.y <= area.y2 + LYRICS_LINE_GAP) {
			int32_t ms = lyrics_cur.lines[i].ms;
			if (ms < 0) {
				return;
			}
			device_state_seek((double)ms / 1000.0);
			// Back to following at once: the finger chose this line.
			lyrics_user_until = 0;
			lyrics_centred = false;
			lyrics_follow((double)ms / 1000.0);
			lyrics_tap_ms = ms;
			lyrics_tap_until = lv_tick_get() + LYRICS_TAP_WAIT_MS;
			return;
		}
	}
}

static void lyrics_build(void) {
	lyrics_pane = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(lyrics_pane);
	lv_obj_set_ignore_layout(lyrics_pane, true);
	lv_obj_set_scrollable(lyrics_pane, false);
	// Presses on the page outside the column go to the sleeve under it.
	lv_obj_set_clickable(lyrics_pane, false);
	lv_obj_set_hidden(lyrics_pane, true);

	lyrics_view = lv_obj_create(lyrics_pane);
	lv_obj_remove_style_all(lyrics_view);
	lv_obj_set_ignore_layout(lyrics_view, true);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(lyrics_view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(lyrics_view, LYRICS_LINE_GAP, 0);
	lv_obj_set_style_pad_hor(lyrics_view, 8, 0);
	lv_obj_set_scroll_dir(lyrics_view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(lyrics_view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(lyrics_view, lyrics_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t) false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t) false);

	lyrics_note = lv_label_create(lyrics_pane);
	lv_obj_set_ignore_layout(lyrics_note, true);
	lv_label_set_long_mode(lyrics_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(lyrics_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_note, &font_ui_24, 0);
	lv_obj_set_style_text_color(lyrics_note, lv_color_white(), 0);
	lv_obj_set_style_text_opa(lyrics_note, LV_OPA_70, 0);
	lv_obj_set_hidden(lyrics_note, true);

	// The title and artist at the top, Studio's head, over the whole screen and
	// above the sleeve. Copies of the two in the row below, which keeps its
	// place so the controls do not move.
	lyrics_head = lv_obj_create(player_screen);
	lv_obj_remove_style_all(lyrics_head);
	lv_obj_set_ignore_layout(lyrics_head, true);
	lv_obj_set_scrollable(lyrics_head, false);
	lv_obj_set_clickable(lyrics_head, false);
	lv_obj_set_hidden(lyrics_head, true);

	lv_obj_t *col = lv_obj_create(lyrics_head);
	lyrics_head_col = col;
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, cover_box_w - 2 * STUDIO_MARGIN - 2 * 68, LV_SIZE_CONTENT);
	lv_obj_align(col, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 4, 0);
	lv_obj_set_scrollable(col, false);
	lv_obj_set_clickable(col, false);

	lyrics_head_title = lv_label_create(col);
	lv_obj_set_width(lyrics_head_title, lv_pct(100));
	lv_obj_set_style_text_align(lyrics_head_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_head_title, lv_obj_get_style_text_font(song_title_label, 0), 0);
	lv_obj_set_style_text_color(lyrics_head_title, lv_color_white(), 0);
	scrolltext_apply(lyrics_head_title);

	lyrics_head_artist = lv_label_create(col);
	lv_obj_set_width(lyrics_head_artist, lv_pct(100));
	lv_obj_set_style_text_align(lyrics_head_artist, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_head_artist, lv_obj_get_style_text_font(song_artist_label, 0), 0);
	lv_obj_set_style_text_color(lyrics_head_artist, lv_color_make(200, 200, 200), 0);
	scrolltext_apply(lyrics_head_artist);

	// The sleeve is where the page is pulled in from.
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t) true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t) true);
}

// Studio's background for the other two arrangements -- the sleeve blurred at
// the shape of the screen, the right way up -- made off the interface thread
// from a copy of the sleeve on screen: a few hundred milliseconds of this
// processor, and it has to be ready before the words are first pulled in.
typedef struct {
	unsigned generation;
	int w, h; // the screen
	cover_image_t src;
	cover_image_t out;
} lyrics_bg_job_t;

static void lyrics_bg_arrived(void *user) {
	lyrics_bg_job_t *job = user;
	if (job->generation == lyrics_bg_generation && job->out.pixels) {
		cover_free(&lyrics_screen_pic);
		lyrics_screen_pic = job->out;
		lyrics_screen_pic.dsc.data = lyrics_screen_pic.pixels; // struct moved, buffer didn't
		if (lyrics_progress > 0) {
			lyrics_look(true);
		}
	} else {
		cover_free(&job->out);
	}
	free(job);
}

static void *lyrics_bg_worker(void *arg) {
	lyrics_bg_job_t *job = arg;
	cover_blur_fill(&job->src, job->w, job->h, &job->out);
	cover_free(&job->src);
	if (!gui_post(lyrics_bg_arrived, job)) {
		cover_free(&job->out);
		free(job);
	}
	return NULL;
}

// Drops the blur that belongs to the sleeve going away, and Studio's look
// with it while the words are up.
static void lyrics_bg_forget(void) {
	lyrics_bg_generation++;
	if (lyrics_look_on && !layout_studio_now) {
		lyrics_look(false);
	}
	if (!layout_studio_now && studio_bg && lv_image_get_src(studio_bg) == &lyrics_screen_pic.dsc) {
		lv_image_set_src(studio_bg, NULL);
	}
	cover_free(&lyrics_screen_pic);
}

// Starts the blur of the sleeve just put up. Not in Studio, which has its own
// blurred copy behind the whole screen already.
static void lyrics_bg_prepare(void) {
	if (layout_studio_now || !current_cover.pixels) {
		return;
	}
	lyrics_bg_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	size_t size = current_cover.dsc.data_size;
	job->src = current_cover;
	job->src.pixels = malloc(size);
	if (!job->src.pixels) {
		free(job);
		return;
	}
	memcpy(job->src.pixels, current_cover.pixels, size);
	job->src.dsc.data = job->src.pixels;
	job->generation = lyrics_bg_generation;
	job->w = (int)lv_obj_get_width(player_screen);
	job->h = (int)lv_obj_get_height(player_screen);

	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_bg_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		cover_free(&job->src);
		free(job);
	}
}

// Which of the four quality marks belongs to what is playing.
//
// Asked of the stream and not of the library, so a file that has not been
// indexed yet is answered as well as one that has.
static const lv_image_dsc_t *studio_quality_mark(void) {
	if (audio_get_dsd_multiple() > 0) {
		return &icon_quality_dsd;
	}
	if (audio_stream_is_lossy()) {
		return &icon_quality_lossy;
	}
	int rate = 0, channels = 0;
	audio_get_stream_info(&rate, &channels);
	int bits = audio_get_stream_bits();
	// CD is 16 bit at 44.1 kHz or below, as in the library lists
	// (library_track_quality); 48 kHz and up is Hi-Res.
	if (rate > 44100 || bits > 16) {
		return &icon_quality_hifi;
	}
	if (rate > 0) {
		return &icon_quality_cd;
	}
	return NULL;
}

// The top of the sleeve, and how large it is.
//
// The sleeve is as large as the narrower of the two constraints allows: the
// width left between the margins, and the height left between the head and the
// line under it, which on this panel is what decides.
//
// Worked out from the numbers rather than from the widgets, so it can be asked
// before the panel has been laid out -- which is where the artwork is ordered
// at the size this arrangement will draw it.
static int studio_cover_geometry(int *top_out) {
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	if (head_top < 2) {
		head_top = 2;
	}

	int top = head_top + STUDIO_HEAD_H + STUDIO_COVER_GAP;
	// Never wider than STUDIO_COVER_MAX_PCT of the panel: where the height
	// allows the full width (the R1), a sleeve from edge to edge crowds the
	// title above it and reads as the standard arrangement.
	int size = studio_box_w * STUDIO_COVER_MAX_PCT / 100;
	int room = studio_box_h - top - STUDIO_QUALITY_GAP - STUDIO_QUALITY_H - STUDIO_BOTTOM;
	if (size > room) {
		size = room;
	}
	if (size < 64) {
		size = 64;
	}
	if (top_out) {
		*top_out = top + (room - size) / 2;
	}
	return size;
}

// Where everything goes.
static void studio_place(void) {
	if (!studio_box) {
		return;
	}

	int screen_w = studio_box_w;
	int head_top = back_btn_centre_y() - STUDIO_HEAD_H / 2;
	if (head_top < 2) {
		head_top = 2;
	}

	int cover_y = 0;
	int size = studio_cover_geometry(&cover_y);
	studio_cover_size = size;

	int cover_x = (screen_w - size) / 2;

	lv_obj_set_pos(studio_head, STUDIO_MARGIN, head_top);
	lv_obj_set_size(studio_head, screen_w - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);

	lv_obj_set_size(studio_cover, size, size);
	lv_obj_set_pos(studio_cover, cover_x, cover_y);

	// The same square, so a track with no artwork keeps the arrangement rather
	// than leaving a bare screen.
	lv_obj_set_size(studio_empty, size, size);
	lv_obj_set_pos(studio_empty, cover_x, cover_y);

	lv_obj_set_size(studio_quality, size, STUDIO_QUALITY_H);
	lv_obj_set_pos(studio_quality, cover_x, cover_y + size + STUDIO_QUALITY_GAP);

	lyrics_layout();
}

// Points the sleeve and the background at the pictures the page already holds.
static void studio_refresh_cover(void) {
	if (!studio_cover) {
		return;
	}
	bool have = current_cover.pixels != NULL;
	lv_image_set_src(studio_cover, have ? &current_cover.dsc : NULL);
	if (have) {
		lv_obj_set_hidden(studio_cover, false);
	} else {
		lv_obj_set_hidden(studio_cover, true);
	}

	// One of the two squares is always up. Without this a track with no artwork
	// left the arrangement with nothing in it at all: no sleeve, no blurred
	// background behind it, and the page's own colour across the whole screen.
	if (studio_empty) {
		if (have) {
			lv_obj_set_hidden(studio_empty, true);
		} else {
			lv_obj_set_hidden(studio_empty, false);
		}
	}
	if (studio_empty_icon) {
		// The same mark the full-width cover panel stands in with.
		lv_image_set_src(studio_empty_icon, lv_image_get_src(cover_placeholder_icon));
		lv_obj_set_style_image_recolor(studio_empty_icon, theme()->text_secondary, 0);
	}

	if (studio_bg) {
		lv_image_set_src(studio_bg, current_backdrop.pixels ? &current_backdrop.dsc : NULL);
		if (current_backdrop.pixels) {
			lv_obj_set_hidden(studio_bg, false);
		} else {
			lv_obj_set_hidden(studio_bg, true);
		}
	}
}

// In the standard arrangement the title is lowered until the middle of its
// capitals meets the middle of the star beside it. Only the title moves: the
// star, the format under it and the artist stay where the row puts them, and a
// translation is not layout, so nothing else on the page moves either. The
// middle of the capitals is read from the font, so it holds at either text
// size. In the pills and in Studio the title is not moved.
static void align_title_with_star(void) {
	if (!song_title_label) {
		return;
	}
	int32_t shift = 0;
	if (song_text_obj && song_side_obj && fav_btn_obj && lv_obj_get_parent(song_title_label) == song_text_obj &&
		lv_obj_get_parent(fav_btn_obj) == song_side_obj && !lv_obj_is_hidden(fav_btn_obj) &&
		!lv_obj_is_hidden(song_side_obj)) {
		const lv_font_t *font = lv_obj_get_style_text_font(song_title_label, 0);
		int32_t line = lv_font_get_line_height(font);
		int32_t title_mid = line / 2;
		lv_font_glyph_dsc_t g;
		if (lv_font_get_glyph_dsc(font, &g, 'H', 0) && g.box_h > 0) {
			// A glyph's box is placed box_h + ofs_y above the baseline, which is
			// base_line up from the bottom of the line.
			int32_t cap_top = line - font->base_line - g.box_h - g.ofs_y;
			title_mid = cap_top + g.box_h / 2;
		}
		// Where the row put the title, without the shift already on it.
		lv_area_t title, star;
		lv_obj_get_coords(song_title_label, &title);
		lv_obj_get_coords(fav_btn_obj, &star);
		int32_t title_top = title.y1 - lv_obj_get_style_translate_y(song_title_label, 0) +
							lv_obj_get_style_pad_top(song_title_label, 0);
		shift = (star.y1 + star.y2) / 2 - (title_top + title_mid);
	}
	// Only when it changes: a new translation lays the column out again, and
	// that comes back here.
	if (lv_obj_get_style_translate_y(song_title_label, 0) != shift) {
		lv_obj_set_style_translate_y(song_title_label, shift, 0);
	}
}

static void song_info_layout_cb(lv_event_t *e) {
	(void)e;
	align_title_with_star();
}

// Puts the ellipsis and the star back where every other arrangement keeps them,
// and takes the panel down. Called before each arrangement is laid out, so the
// two that know nothing about Studio find the page as they left it.
static void studio_take_back(void) {
	if (!studio_box) {
		return;
	}
	lv_obj_set_hidden(studio_box, true);
	if (studio_bg) {
		lv_obj_set_hidden(studio_bg, true);
	}
	if (studio_empty) {
		lv_obj_set_hidden(studio_empty, true);
	}

	// The surfaces this arrangement made transparent, and the light chrome that
	// goes with the blurred sleeve. Only put back by the arrangement that took
	// them away: every other path leaves the cover to decide, as it always has.
	if (studio_up) {
		studio_up = false;
		lv_obj_remove_local_style_prop(cover_panel, LV_STYLE_BG_OPA, 0);
		lv_obj_set_height(cover_panel, cover_box_h);
		if (player_menu) {
			lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_OPA, 0);
			lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_IMAGE_OPA, 0);
			lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_set_style_pad_ver(player_menu, menu_pad_ver, 0);
			lv_obj_set_style_pad_gap(player_menu, menu_gap, 0);
		}
		// The picture at full width comes back, and the note with it when there
		// is no picture. studio_up is already down, so cover_show() agrees.
		cover_show(current_cover.pixels != NULL);
		if (format_label && song_side_obj) {
			lv_obj_set_parent(format_label, song_side_obj);
			lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_RIGHT, 0);
		}
		set_over_cover(current_cover.pixels != NULL);
	}

	if (more_btn_obj && controls_row) {
		lv_obj_set_parent(more_btn_obj, controls_row);
		lv_obj_set_ignore_layout(more_btn_obj, true);
		lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	if (fav_btn_obj) {
		// Laid out by its row again: the next step decides which row that is.
		lv_obj_set_ignore_layout(fav_btn_obj, false);
	}
}

static void studio_put(void) {
	if (!studio_box) {
		return;
	}

	if (song_title_label) {
		lv_obj_set_parent(song_title_label, studio_text_col);
		lv_obj_move_to_index(song_title_label, 0);
		lv_obj_set_width(song_title_label, lv_pct(100));
		lv_obj_set_style_max_width(song_title_label, LV_COORD_MAX, 0);
	}
	if (song_artist_label) {
		lv_obj_set_parent(song_artist_label, studio_text_col);
		lv_obj_move_to_index(song_artist_label, 1);
		lv_obj_set_width(song_artist_label, lv_pct(100));
		lv_obj_set_style_max_width(song_artist_label, LV_COORD_MAX, 0);
	}
	lv_obj_set_hidden(song_text_obj, true);
	if (song_side_obj) {
		lv_obj_set_hidden(song_side_obj, true);
	}

	// The line under the sleeve is the same label the standard arrangement
	// keeps beside the artist -- it already knows how to say "16/44.1 FLAC",
	// "320 kbps MP3" and "DSD256". Moved, not copied.
	if (format_label) {
		lv_obj_set_parent(format_label, studio_quality);
		lv_obj_set_hidden(format_label, false);
		lv_obj_set_width(format_label, LV_SIZE_CONTENT);
		lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_LEFT, 0);
	}

	// The two buttons change places: the ellipsis belongs beside the title it
	// is about, and the star takes the corner of the transport row it left.
	if (more_btn_obj && studio_head) {
		lv_obj_set_parent(more_btn_obj, studio_head);
		lv_obj_set_ignore_layout(more_btn_obj, true);
		lv_obj_align(more_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}
	if (fav_btn_obj && controls_row) {
		lv_obj_set_parent(fav_btn_obj, controls_row);
		lv_obj_set_ignore_layout(fav_btn_obj, true);
		lv_obj_align(fav_btn_obj, LV_ALIGN_RIGHT_MID, 0, 0);
	}

	// The blurred sleeve is the whole background here, so everything that would
	// otherwise cover it gets out of the way: the two panels' own fills, the
	// picture at full width, and the note that stands in for it.
	studio_up = true;
	lv_obj_set_style_bg_opa(cover_panel, LV_OPA_TRANSP, 0);
	// The panel with the sleeve reaches down over the top of the controls
	// block, which in this arrangement holds its rows at the bottom. Grown
	// rather than left to overflow: a band of the screen redrawn below the
	// panel's own area would skip it and cut the sleeve off.
	lv_obj_set_height(cover_panel, studio_box_h);
	if (player_menu) {
		lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(player_menu, PLAYER_MENU_PAD_VER, 0);
		lv_obj_set_style_pad_gap(player_menu, menu_gap, 0);
		lv_obj_set_style_bg_opa(player_menu, LV_OPA_TRANSP, 0);
		// Its own blurred copy too: it is the same picture at a different crop,
		// and two of them meeting at the controls is a seam across the screen.
		lv_obj_set_style_bg_image_opa(player_menu, LV_OPA_TRANSP, 0);
	}
	cover_show(current_cover.pixels != NULL);

	lv_obj_set_hidden(studio_box, false);
	lv_obj_move_foreground(studio_box);
	if (studio_bg) {
		lv_obj_move_background(studio_bg);
	}
	studio_place();
	studio_refresh_cover();
}

// Moves the pieces between the two arrangements.
//
// Reparenting and not rebuilding: the title label carries its scrolling, the
// star carries what it does when pressed, and copying either of them would be
// copying behaviour.
static void apply_layout(void) {
	if (!alt_title_pill || !song_text_obj) {
		return;
	}

	// Always first: the two arrangements below know nothing about Studio, so
	// they have to find the ellipsis and the star where they left them.
	studio_take_back();

	if (layout_alt_now) {
		// A pill each, and not one pill with two lines in it: the title and the
		// album's artist are two different things, a single box around both
		// pads the shorter one out to the length of the longer, and each has to
		// be able to scroll on its own.
		//
		// The labels are a percentage of their parent in the standard layout,
		// and a percentage of a parent that is itself sized by its contents is
		// nothing at all. Here they size themselves -- so each pill ends where
		// its own text ends -- up to what is left of the sleeve once the star's
		// disc and the margins are taken off.
		int room = cover_box_w - 2 * ALT_PAD - ALT_FAV_SIZE - 2 * ALT_PILL_PAD_H - 24;
		if (song_title_label) {
			lv_obj_set_parent(song_title_label, alt_title_pill);
			lv_obj_set_width(song_title_label, LV_SIZE_CONTENT);
			lv_obj_set_style_max_width(song_title_label, room, 0);
		}
		if (song_artist_label) {
			lv_obj_set_parent(song_artist_label, alt_artist_pill);
			lv_obj_set_width(song_artist_label, LV_SIZE_CONTENT);
			lv_obj_set_style_max_width(song_artist_label, room, 0);
		}

		if (fav_btn_obj && alt_fav_circle) {
			lv_obj_set_parent(fav_btn_obj, alt_fav_circle);
			lv_obj_center(fav_btn_obj);
		}
		if (format_label) {
			lv_obj_set_hidden(format_label, true); // the sleeve is not the place for a bitrate
		}
		if (song_side_obj) {
			lv_obj_set_hidden(song_side_obj, true);
		}
		lv_obj_set_hidden(song_text_obj, true); // empty now, and it would still take a row

		lv_obj_set_hidden(alt_text_col, false);
		lv_obj_set_hidden(alt_fav_circle, false);
		// Over the sleeve and not under it: both were built before the picture
		// so that the picture would not have to be rebuilt around them.
		lv_obj_move_foreground(alt_text_col);
		lv_obj_move_foreground(alt_fav_circle);
		if (wave_canvas) {
			lv_obj_set_hidden(wave_box, false);
			lv_obj_move_to_index(wave_box, lv_obj_get_index(progress_slider));
			slider_over_waveform(true);
		}
	} else {
		if (song_title_label) {
			lv_obj_set_parent(song_title_label, song_text_obj);
			lv_obj_move_to_index(song_title_label, 0);
			lv_obj_set_width(song_title_label, lv_pct(100));
			lv_obj_set_style_max_width(song_title_label, LV_COORD_MAX, 0);
		}
		if (song_artist_label) {
			lv_obj_set_parent(song_artist_label, song_text_obj);
			lv_obj_move_to_index(song_artist_label, 1);
			lv_obj_set_width(song_artist_label, lv_pct(100));
			lv_obj_set_style_max_width(song_artist_label, LV_COORD_MAX, 0);
		}
		lv_obj_set_hidden(song_text_obj, false);

		if (fav_btn_obj && song_side_obj) {
			lv_obj_set_parent(fav_btn_obj, song_side_obj);
			lv_obj_move_to_index(fav_btn_obj, 0);
		}
		if (format_label) {
			lv_obj_set_hidden(format_label, false);
		}
		if (song_side_obj) {
			lv_obj_set_hidden(song_side_obj, false);
		}

		lv_obj_set_hidden(alt_text_col, true);
		lv_obj_set_hidden(alt_fav_circle, true);
		if (layout_wave_now && wave_canvas) {
			lv_obj_set_hidden(wave_box, false);
			lv_obj_move_to_index(wave_box, lv_obj_get_index(progress_slider));
			slider_over_waveform(true);
		} else {
			slider_over_waveform(false);
			if (wave_box) {
				lv_obj_set_hidden(wave_box, true);
			}
		}
	}

	// Bigger targets in this arrangement. The star's button is as big as the
	// disc it sits in, which is what a finger aims at; a press beyond the disc
	// could not reach it anyway, since LVGL looks for a child only inside its
	// parent.
	if (fav_btn_obj) {
		lv_obj_set_size(fav_btn_obj, layout_alt_now ? ALT_FAV_SIZE : FAV_BTN_W, layout_alt_now ? ALT_FAV_SIZE : FAV_BTN_H);
	}
	lv_obj_t *const transport[] = {prev_btn_obj, play_btn, next_btn_obj};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (transport[i]) {
			lv_obj_set_ext_click_area(transport[i], layout_alt_now ? ALT_EXT_CLICK : 0);
		}
	}

	if (layout_studio_now) {
		studio_put();
	} else {
		lyrics_layout(); // studio_put() places the page itself
	}
	align_title_with_star(); // the title may have just left its row, or come back to it

	paint_alt_tint();
	bar_over_player_update(); // Studio's bar blurs nothing of its own
}

// Decides which arrangement the thing now playing gets, and moves the pieces
// only if the answer changed.
//
// Called from every now-playing refresh and from the poll, which is what makes
// the arrangement follow the source instead of being settled once at startup.
// The bar's parent, size and transparency are decided here and nowhere else.
static void update_layout(const device_state_t *state) {
	// Studio only wants a picture and a name, and takes whatever is playing.
	bool local = playing_local_file(state);
	bool lyrics = lyrics_wanted && local;
	bool studio = layout_choice == PLAYER_LAYOUT_STUDIO;
	bool wave = layout_choice == PLAYER_LAYOUT_ALTERNATIVE && local;
	bool alt = wave;

	if (studio == layout_studio_now && alt == layout_alt_now && wave == layout_wave_now && lyrics == lyrics_allowed &&
		alt_title_pill) {
		return;
	}
	layout_studio_now = studio;
	layout_alt_now = alt;
	layout_wave_now = wave;
	lyrics_allowed = lyrics;
	// Up only with words for this very file; while they are still being read,
	// as they were, so a track change does not take the page down and up again.
	bool same_file = strcmp(state->current_file, lyrics_path) == 0;
	if (!lyrics) {
		lyrics_now = false;
	} else if (same_file && lyrics_loaded) {
		lyrics_now = lyrics_cur.count > 0;
	}
	apply_layout();
	lyrics_request(state);

	// The blurred copy is made at the shape of whatever shows it, so moving in
	// or out of Studio means the one on hand is the wrong shape and the picture
	// is asked for again. Only on a change of arrangement, which is a setting
	// the user has just touched.
	if (studio != backdrop_is_studio && cover_shown_path[0]) {
		reload_cover(cover_shown_path);
	}
}

// ---------------------------------------------------------------------------
// the shape of the track
// ---------------------------------------------------------------------------

// Paints the bars into the canvas buffer by hand.
//
// RGB565A8, and written directly rather than through the drawing pipeline: the
// gaps between the bars have to let the blurred sleeve through, an RGB565
// canvas would lay an opaque rectangle over it, and one LVGL object per bar is
// one more thing to lay out on every track change.
//
// The buffer is the colour plane followed by the alpha plane, which is what
// LVGL means by RGB565A8.
//
// Each bar is a capsule rather than a rectangle: `cap` is how far the drawn
// column is pulled in at that horizontal offset, from the circle of diameter
// `solid` that rounds the ends. A silent stretch is then a row of dots rather
// than a row of ticks, which is the shape this player's buttons and pills are
// already drawn in.
// The bars all start at the same place and are all the same width, and two
// callers besides the painter need to know where: one to work out how far along
// the playhead is in pixels, the other to compare that with what is on screen.
static void wave_geometry(int *bar_w, int *left) {
	int w = wave_w;
	int b = w / WAVEFORM_BARS;
	if (b < 1) {
		b = 1;
	}
	*bar_w = b;
	*left = (w - b * WAVEFORM_BARS) / 2;
}

// How far along the strip the playhead is, in pixels of the canvas.
//
// Pixels and not columns: a column is a forty-eighth of the track, so a column
// boundary stands still for several seconds and then jumps. In pixels the
// playhead moves by one every half second or so, and the bar it is inside is
// lit up to the playhead rather than all at once.
//
// Along the same stretch as the bar laid over it, so a finger
// on the strip lands where the lit part says it will.
static int wave_played_px(double position) {
	int bar_w, left;
	wave_geometry(&bar_w, &left);
	double fraction = view_length > 0.1 ? view_value(position) / 1000.0 : 0.0;
	return left + (int)(fraction * bar_w * WAVEFORM_BARS);
}

static void wave_paint(int played_px) {
	if (!wave_buf || !wave_canvas) {
		return;
	}

	// The buffer's own width, and never the object's: an object that has not
	// been laid out yet answers with something else, and painting to that
	// number writes past the end of the buffer.
	int w = wave_w;
	int h = WAVE_HEIGHT;
	if (w <= 0) {
		return;
	}

	uint16_t *colour = (uint16_t *)wave_buf;
	uint8_t *alpha = wave_buf + (size_t)w * h * 2;
	memset(alpha, 0, (size_t)w * h);

	lv_color_t tint = alt_mark();
	uint16_t packed = (uint16_t)(((tint.red & 0xF8) << 8) | ((tint.green & 0xFC) << 3) | (tint.blue >> 3));

	int bar_w, left;
	wave_geometry(&bar_w, &left);
	int solid = bar_w - WAVE_BAR_GAP;
	if (solid < 1) {
		solid = 1;
	}
	int middle = h / 2;

	// The tallest a bar may be drawn, with the cap and a pixel of air left over
	// so that a loud passage does not end flat against the top edge -- which is
	// exactly what reads as clipped audio.
	int reach = h / 2 - 1 - solid / 2;
	if (reach < WAVE_MIN_BAR) {
		reach = WAVE_MIN_BAR;
	}

	for (int i = 0; i < WAVEFORM_BARS; i++) {
		int value = i < wave_ready ? wave_bars[i] : 0;
		int half = value * reach / 255;
		if (half < WAVE_MIN_BAR) {
			half = WAVE_MIN_BAR;
		}
		int x0 = left + i * bar_w;
		for (int dx = 0; dx < solid; dx++) {
			int x = x0 + dx;
			if (x < 0 || x >= w) {
				continue;
			}
			// Decided per column of pixels rather than per bar, which is what
			// lets the playhead stand in the middle of a bar.
			uint8_t opa = x < played_px ? WAVE_PAST_OPA : WAVE_TODO_OPA;
			// Doubled coordinates, so that an even-width bar can have its
			// centre between two pixels without any rounding.
			int d2 = 2 * dx - (solid - 1);
			int cap = (solid - (int)utils_isqrt32((uint32_t)(solid * solid - d2 * d2))) / 2;

			for (int y = middle - half + cap; y <= middle + half - cap; y++) {
				if (y < 0 || y >= h) {
					continue;
				}
				size_t at = (size_t)y * w + x;
				colour[at] = packed;
				alpha[at] = opa;
			}
		}
	}

	lv_obj_invalidate(wave_canvas);
}

// Moves the played part of the shape to `seconds`, if that has moved it at all.
//
// The guard is what keeps this cheap enough to call at fifteen frames a second:
// repainting is eighty kilobytes of buffer and an invalidate, and on a track of
// any length the answer changes about twice a second.
static void wave_preview(double seconds) {
	if (!layout_wave_now || !wave_canvas || view_length <= 0.1) {
		return;
	}
	int px = wave_played_px(seconds);
	if (px == wave_drawn) {
		return;
	}
	wave_drawn = px;
	wave_paint(px);
}

// Asks for this track's shape and repaints when either the shape or the
// playhead has moved. Called from the progress poll, so it runs twice a second
// while something is playing and once every two seconds otherwise.
static void wave_refresh(const char *path, double position) {
	if (!layout_wave_now || !wave_canvas) {
		return;
	}

	// Compared with what is on screen as well as counted: while the shape is
	// being worked out the columns already shown are scaled again whenever a
	// louder one arrives.
	int had = wave_ready;
	uint8_t before[WAVEFORM_BARS];
	memcpy(before, wave_bars, sizeof(before));
	if (path && path[0]) {
		wave_ready = waveform_get(path, wave_bars);
	} else {
		wave_ready = 0;
	}
	bool same_shape = wave_ready == had && memcmp(before, wave_bars, (size_t)wave_ready) == 0;

	int px = wave_played_px(position);
	uint32_t tone = album_tone;
	if (px == wave_drawn && same_shape && tone == wave_drawn_tone) {
		return;
	}
	wave_drawn = px;
	wave_drawn_tone = tone;
	wave_paint(px);
}

// The size the sleeve is asked for in the arrangement in force (see
// reload_cover()).
static void cover_request_size(int *w, int *h) {
	*w = layout_studio_now ? studio_cover_geometry(NULL) : cover_box_w;
	*h = layout_studio_now ? *w : cover_box_h;
}

// The next track's cover, made and kept in the cache while this one plays, so
// that skipping to it, or reaching it, finds it ready instead of decoding it
// then. Only with the screen on: with it off nobody sees the cover arrive, and
// the decode would be battery spent on a guess.
static void prefetch_next_cover(void) {
	if (!power_screen_is_on()) {
		return;
	}
	char next[512];
	if (!playlist_peek_auto(next, sizeof(next)) || strcmp(next, cover_shown_path) == 0) {
		return;
	}
	int w, h;
	cover_request_size(&w, &h);
	coverloader_prefetch_player(next, w, h);
}

// Throws away what is on screen and asks for the artwork of `filepath`. The
// half of refresh_cover() that runs once it is known that the picture really is
// changing.
static void reload_cover(const char *filepath) {
	if (cover_release_cb) {
		cover_release_cb();
	}

	// Detach the old pixels before freeing them, so nothing can draw from a
	// buffer that's already gone. The placeholder covers the gap while the
	// worker decodes.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(player_menu, NULL, 0);
	// The blur of the picture going away goes with it, from behind the words
	// too: until the new one is made they are on a plain dark page.
	lyrics_bg_forget();
	if (layout_studio_now && studio_bg) {
		lv_image_set_src(studio_bg, NULL); // Studio's points at the backdrop freed below
	}
	cover_free(&current_cover);
	cover_free(&current_backdrop);
	bar_over_player_update();

	cover_show(false);
	// Studio holds pointers to both of those, so it is told in the same breath:
	// its sleeve and its background come down and the empty square goes up for
	// as long as the decode takes.
	if (layout_studio_now) {
		studio_refresh_cover();
	}

	// Backdrop gone: the chrome goes back to following the theme.
	set_over_cover(false);
	album_tone = 0;
	paint_alt_tint();

	lv_obj_invalidate(cover_panel);
	lv_obj_invalidate(player_menu);

	if (filepath && filepath[0]) {
		backdrop_is_studio = layout_studio_now;
		cover_set_backdrop_upright(backdrop_is_studio);

		// The sleeve is asked for at the size the arrangement in force will
		// draw it. A picture that does not match its widget is resampled by
		// LVGL on every frame it is drawn, and every frame is what a sheet
		// being dragged, or the control centre coming down over it, means: in
		// Studio the picture is decoded at the width of the screen and shown
		// at two thirds of it, and that resampling is the interface's core
		// being spent again and again on an answer that never changes.
		int cover_w, cover_h;
		cover_request_size(&cover_w, &cover_h);
		coverloader_request_player(filepath, cover_w, cover_h, backdrop_w,
								   backdrop_is_studio ? backdrop_studio_h : backdrop_h);
		cover_request_outstanding = true;

		// The collector rides on the progress timer too; make sure it is
		// ticking even when nothing is playing (paused player, theme change).
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
	} else {
		cover_request_outstanding = false;
	}
}

// A track change.
//
// The next track of a record carries the record's cover, and so does the next
// file in a folder that keeps its picture beside the music. Decoding it again
// is a second of this processor's time for a picture that does not change, and
// the cover blinking out to the placeholder and back is the part the user sees.
//
// So nothing is thrown away yet: the loader is asked what the new track's
// artwork is -- which it answers by reading the picture, not by decoding it --
// and only an answer that differs from what is on screen costs anything. When
// it does differ the old cover stays up for the one poll that takes, which
// still reads better than a placeholder in the gap.
static void refresh_cover(const char *filepath) {
	if (!cover_img)
		return;

	snprintf(cover_shown_path, sizeof(cover_shown_path), "%s", filepath ? filepath : "");

	if (cover_shown_id != 0 && current_cover.pixels && filepath && filepath[0]) {
		cover_id_state = COVER_ID_NEW_TRACK;
		coverloader_request_player_id(filepath);
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
		return;
	}

	cover_id_state = COVER_ID_IDLE;
	cover_incoming_id = 0;
	reload_cover(filepath);
}

// The answer to whichever question was asked, picked up on the progress poll.
static void apply_cover_id_result(void) {
	if (cover_id_state == COVER_ID_IDLE) {
		return;
	}
	uint64_t id = 0;
	bool finished = false;
	coverloader_take_player_id(&id, &finished);
	if (!finished) {
		return;
	}

	if (cover_id_state == COVER_ID_MEASURE) {
		cover_id_state = COVER_ID_IDLE;
		cover_shown_id = id;
		return;
	}

	cover_id_state = COVER_ID_IDLE;

	// The same picture: the cover, the backdrop, the sleeve colour and the
	// chrome all stay exactly as they are. This is the whole point.
	if (id != 0 && id == cover_shown_id) {
		return;
	}

	// Different, and its id is already known: the decode does not have to be
	// asked what it produced.
	cover_incoming_id = id;
	reload_cover(cover_shown_path);
}

// Picks up the worker's finished artwork, if any. Runs on the UI thread from
// the progress timer, so touching widgets here is allowed.
static void apply_cover_result(void) {
	if (!cover_request_outstanding) {
		return;
	}

	cover_image_t new_cover, new_backdrop;
	bool finished = false;
	bool has_cover = coverloader_take_player(&new_cover, &new_backdrop, &finished);

	if (!finished) {
		return; // still decoding; the placeholder stays up
	}
	cover_request_outstanding = false;

	if (has_cover) {
		// What is now on screen, so the next track can be asked whether its
		// artwork is this same picture. Usually known already; when it is not
		// -- the first cover after the player opens -- the
		// loader is asked, because working the id out reads the file and this
		// is the interface thread.
		cover_shown_id = cover_incoming_id;
		if (cover_shown_id == 0 && cover_shown_path[0]) {
			cover_id_state = COVER_ID_MEASURE;
			coverloader_request_player_id(cover_shown_path);
			cover_poll_start();
		}

		current_cover = new_cover;
		current_cover.dsc.data = current_cover.pixels; // struct moved, buffer didn't

		lv_image_set_src(cover_img, &current_cover.dsc);
		lv_obj_set_size(cover_img, current_cover.dsc.header.w, current_cover.dsc.header.h);
		lv_obj_center(cover_img);

		cover_show(true);

		if (new_backdrop.pixels) {
			current_backdrop = new_backdrop;
			current_backdrop.dsc.data = current_backdrop.pixels;
			lv_obj_set_style_bg_image_src(player_menu, &current_backdrop.dsc, 0);
		}

		// The dark backdrop is up: light chrome, in both themes.
		set_over_cover(true);

		// And the sleeve's own colour, which is what the alternative layout is
		// drawn in: the one an album's page is painted in (cover_page_tone),
		// so the record looks the same colour on both. Kept with the picture by
		// the loader, so a cover seen before is not worked out again, and taken
		// to full strength here for marks drawn over a dark backdrop.
		uint32_t tone = 0;
		int luma = 0;
		bool light = false;
		if (current_cover.has_tone) {
			tone = current_cover.tone;
		} else if (!cover_page_tone(&current_cover, COVER_PAGE_LIGHT_FROM, &tone, &luma, &light)) {
			tone = 0;
		}
		album_tone = tone ? cover_tone_stretched(tone) : 0;
		paint_alt_tint();

		// The words' background, ready before they are pulled in.
		lyrics_bg_prepare();

		lv_obj_invalidate(cover_panel);
		lv_obj_invalidate(player_menu);
	}
	if (!has_cover) {
		cover_shown_id = 0; // nothing to compare the next track against
		lyrics_bg_forget(); // no picture, nothing to blur
	}
	bar_over_player_update();
	if (lyrics_progress > 0) {
		lyrics_look(true); // Studio's backdrop has arrived, or there is none
	}
	cover_incoming_id = 0;

	// Studio draws both of these itself -- the sleeve smaller and the blurred
	// copy across the whole screen -- so it is told here, where they land,
	// rather than asked on a timer.
	if (layout_studio_now) {
		studio_refresh_cover();
	}
	// No artwork: the placeholder is already showing.

	prefetch_next_cover();
}

// Collects whatever the loader has answered, quickly, for as long as anything
// is outstanding; then the timer stops until the next question.
static void cover_poll_cb(lv_timer_t *timer) {
	apply_cover_id_result();
	apply_cover_result();
	if (!cover_request_outstanding && cover_id_state == COVER_ID_IDLE) {
		lv_timer_pause(timer);
	}
}

static void cover_poll_start(void) {
	if (!cover_poll_timer) {
		cover_poll_timer = lv_timer_create(cover_poll_cb, COVER_POLL_MS, NULL);
	}
	lv_timer_resume(cover_poll_timer);
}

// The artwork the player has already decoded for the loaded track, lent to
// anything else that wants to draw it (the screensaver). NULL while a track
// has no cover or its decode is still in flight -- never a stale pointer:
// refresh_cover() zeroes both before the buffers are freed.
const lv_image_dsc_t *player_cover_image(void) { return current_cover.pixels ? &current_cover.dsc : NULL; }

const lv_image_dsc_t *player_backdrop_image(void) {
	return current_backdrop.pixels ? &current_backdrop.dsc : NULL;
}

static void update_repeat_button(void);

// Repaints the bits that are coloured by hand, and rebuilds the blurred
// artwork behind the controls -- it is dimmed towards the page background, so
// it has to be regenerated when that background changes.
static void player_refresh_theme(void) {
	if (!cover_panel)
		return;

	if (studio_box) {
		lv_obj_set_style_bg_color(studio_box, theme()->screen_bg, 0);
	}

	lv_obj_set_style_bg_color(cover_panel, theme()->cover_bg, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, theme()->text_secondary, 0);
	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	set_over_cover(chrome_over_cover); // repaints the play disc for the new palette
	update_repeat_button();

	device_state_t state;
	device_state_get(&state);
	refresh_cover(state.current_file);
	if (lyrics_pane) {
		lyrics_look(lyrics_look_on); // the words in the new theme's colours
	}
}

// Refreshes the now-playing info (title + artist) from the current
// device state and resets the per-track length cache. Used both when the user
// picks a file and when playback auto-advances to a new track.
static void refresh_now_playing(void) {
	current_total_length = 0; // unknown until the playback thread reports it

	// The library lists carry an accent mark on the playing track (and on the
	// album and artist it belongs to). This is the one place every track
	// change goes through, so it is where they are told.
	medialist_notify_now_playing();
	trackmenu_notify_now_playing();

	// Wind the progress display back to the start straight away, so the bar
	// never sits at the previous track's position while the new one loads.
	lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
	set_progress_label(0, 0);

	device_state_t state;
	device_state_get(&state);

	// Before anything is drawn: which arrangement this source gets decides
	// where the title goes and what colour everything is.
	update_layout(&state);

	const char *file = state.current_file;
	const char *slash = strrchr(file, '/');
	scrolltext_set(song_title_label, state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : file));

	// Whose name goes under the title: see metadata_shown_artist().
	const char *artist = metadata_shown_artist(&state.metadata);
	scrolltext_set(song_artist_label, artist);
	lyrics_head_sync();
	alt_pills_sync();

	update_fav_button();
	update_format_label(&state);

	lyrics_request(&state);

	refresh_cover(file);
}

// Repaints the star from the database: filled yellow when the loaded track
// is a favourite, quiet outline otherwise.
static void update_fav_button(void) {
	if (!fav_btn_icon) {
		return;
	}

	// device_state, not audio_get_current_file(): the star is drawn from one
	// source and toggled from another, and when the two disagreed the star
	// never filled in -- the tap wrote the right track to the database while
	// the drawing looked up a different one.
	device_state_t state;
	device_state_get(&state);
	bool starred = state.current_file[0] && library_fav_contains(state.current_file);

	// The star's colour is decided here and nowhere else. This function runs
	// last on every track change, so anything that tints the star elsewhere is
	// overwritten.
	//
	// An empty star is drawn in the disc's own ink so that it reads on it; a
	// filled one keeps the yellow, which is the same on every page of the
	// player and is what says "favourite" rather than "here is the album's
	// colour again".
	if (starred) {
		lv_image_set_src(fav_btn_icon, &icon_star_filled);
		lv_obj_set_style_image_recolor(fav_btn_icon, lv_color_make(246, 211, 45), 0); // Adwaita yellow
	} else {
		lv_image_set_src(fav_btn_icon, &icon_star);
		lv_obj_set_style_image_recolor(fav_btn_icon, layout_alt_now ? alt_ink() : lv_color_make(150, 150, 150), 0);
	}
	lv_obj_set_style_image_recolor_opa(fav_btn_icon, LV_OPA_COVER, 0);
}

static void fav_btn_event_cb(lv_event_t *e) {
	(void)e;

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	const char *title = state.metadata.title[0] ? state.metadata.title : strrchr(state.current_file, '/') + 1;
	// No notification popup: the star filling in is the whole feedback.
	library_fav_toggle(state.current_file, title, state.metadata.artist);
	update_fav_button();
}

// The format line, the way the stock player writes it: "16/44.1 FLAC".
static void update_format_label(const device_state_t *state) {
	// The mark beside it in Studio, which is the same question asked of the
	// stream rather than of the library. Here because this is where the format
	// line is worked out, and the two say one thing between them.
	if (layout_studio_now && studio_quality_icon) {
		const lv_image_dsc_t *mark = studio_quality_mark();
		if (mark) {
			lv_image_set_src(studio_quality_icon, mark);
			lv_obj_set_hidden(studio_quality_icon, false);
		} else {
			lv_obj_set_hidden(studio_quality_icon, true);
		}
	}

	if (!format_label) {
		return;
	}

	if (state->stream_sample_rate <= 0 || !state->current_file[0]) {
		lv_label_set_text(format_label, "");
		return;
	}

	const char *dot = strrchr(state->current_file, '.');
	char kind[8] = "";
	if (dot) {
		size_t k = 0;
		for (const char *c = dot + 1; *c && k < sizeof(kind) - 1; c++) {
			kind[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
		}
		kind[k] = '\0';
	}

	// A DSD track says what it is rather than what carries it: "24/176.4 DSF"
	// is true of a DSD64 file and of a DSD256 one, and tells the listener
	// nothing. How it gets there is not said: DoP is the only way it does.
	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		lv_label_set_text_fmt(format_label, "DSD%d", dsd);
		return;
	}

	// The codec name comes from the decoder, not the extension: an .m4a holds
	// either AAC or ALAC, and those read as two different things under a
	// cover. When the decoder has no name to give -- the libsndfile formats --
	// the extension still serves.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	const char *name = codec[0] ? codec : kind;

	// A lossy format has no bit depth of its own: "16/44.1 MP3" describes the
	// PCM leaving the decoder, not the file, and is equally true of a 96 kbps
	// MP3 and a 320 kbps one. What an MP3, an AAC or an Opus is is its bitrate.
	if (audio_stream_is_lossy()) {
		// The nominal figure first, the one written in the file.
		int kbps = audio_get_stream_bitrate_kbps();

		// When the file does not declare one (a variable-bitrate MP3, a silent
		// AAC) the real average stands in.
		struct stat st;
		if (kbps <= 0 && state->progress_total_secs > 0.5 && stat(state->current_file, &st) == 0 &&
			st.st_size > 0) {
			kbps = (int)((double)st.st_size * 8.0 / state->progress_total_secs / 1000.0 + 0.5);
		}

		if (kbps > 0) {
			lv_label_set_text_fmt(format_label, "%d kbps %s", kbps, name);
			return;
		}
		// Neither one: the format name alone beats quoting a bit depth the
		// file does not have.
		lv_label_set_text(format_label, name);
		return;
	}

	int bits = audio_get_stream_bits();
	lv_label_set_text_fmt(format_label, "%d/%g %s", bits > 0 ? bits : 16, state->stream_sample_rate / 1000.0, name);
}

// Updates the repeat-mode button's icon and colour to reflect the active mode.
// The icons are the white ARGB bitmaps generated from assets/icons/*.svg;
// recolouring them is what marks the mode as active or not.
static void update_repeat_button(void) {
	// The active mode wears the accent colour of the moment, read each time
	// rather than frozen, so it follows a change of accent.
	lv_color_t active = chrome_accent();
	lv_color_t inactive = lv_color_make(100, 100, 100);

	const lv_image_dsc_t *icon;
	lv_color_t color;

	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_REPEAT_ONE:
		icon = &icon_repeat_one;
		color = active;
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		icon = &icon_repeat_all;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		icon = &icon_shuffle;
		color = active;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		icon = &icon_shuffle_repeat;
		color = active;
		break;
	case PLAYBACK_MODE_NORMAL:
	default:
		icon = &icon_repeat_off;
		color = inactive;
		break;
	}

	lv_image_set_src(repeat_btn_icon, icon);
	lv_obj_set_style_image_recolor(repeat_btn_icon, color, 0);
	lv_obj_set_style_image_recolor_opa(repeat_btn_icon, LV_OPA_COVER, 0);
}

// Called when the current track finished on its own: advances the folder queue
// per the active playback mode. If a next track starts, refresh the UI to it;
// otherwise playback simply stays stopped (end of folder in normal mode).
static void handle_track_finished(void) {
	if (device_state_advance_auto(NULL, 0)) {
		refresh_now_playing();
	} else {
		// The queue ran out: the track it ended on went into the history.
		trackmenu_notify_now_playing();
	}
}

// A track ended on its own: the next one is started now rather than at the
// next poll, while the PCM held for gapless still has its queue to play.
static void track_end_async(void *user) {
	(void)user;
	if (device_state_take_completion()) {
		handle_track_finished();
	}
}

// Playback thread.
static void track_end_hook(void) { gui_post(track_end_async, NULL); }

// Reads the current device state and reconciles the UI against it. This is the
// single point that keeps the play/pause button and the progress bar from
// going stale once a track finishes on its own.
static void update_progress(void) {
	// Auto-advance or loop the folder when the track has played through.
	if (device_state_take_completion()) {
		handle_track_finished();
	}

	device_state_t state;
	device_state_get(&state);

	// Asked on every poll and not only at a track change, for the routes that
	// are not one: the answer here has to follow whatever moves under the
	// track that is loaded. Two string compares and a struct copy, twice a
	// second.
	update_layout(&state);

	if (state.progress_total_secs > 0) {
		current_total_length = state.progress_total_secs;
	}

	// The truth the smoothing works from, and the moment it was true. Taken
	// here and not in the smoothing itself, so the two can never drift: every
	// poll puts the interpolation back on the playback thread's own figure.
	progress_anchor_secs = state.progress_current_secs;
	progress_anchor_tick = lv_tick_get();
	progress_running = state.status == AUDIO_STATUS_PLAYING && state.progress_total_secs > 0;
	smooth_timer_sync();

	set_progress_label(state.progress_current_secs, current_total_length);
	wave_refresh(cover_shown_path, state.progress_current_secs);
	if (state.status == AUDIO_STATUS_PLAYING) {
		device_state_read_ahead(state.progress_current_secs, state.progress_total_secs);
	}
	lyrics_request(&state);
	lyrics_follow(state.progress_current_secs);
	apply_playback_status(state.status);
	update_format_label(&state); // the rate/bits settle shortly after the start

	// The status LED follows playback promptly from here (the battery poll
	// only comes round once a minute). Only writes when the colour changes.
	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate,
						audio_get_dsd_multiple() > 0);

	// "Remember track". The decision, and the writing, live in device_state.c:
	// the power menu and the automatic shutdown have to do the same thing on
	// their way out, and neither of them comes through here.
	device_state_remember_note();
}

static void play_btn_event_cb(lv_event_t *e) {
	(void)e;

	audio_status_t new_status = device_state_toggle_play_pause();
	apply_playback_status(new_status); // optimistic UI update; next poll reconciles against the real status
}

// What the on-screen prev/next buttons do: previous/next, Spotify style --
// within the first few seconds of a track prev moves to the previous one, later
// it restarts the current one. Public so anything else that wants the same
// buttons can do exactly this: they cannot mean two different things in two
// places.
void player_screen_prev(void) {
	device_state_t state;
	device_state_get(&state);

	// Past the first three seconds, prev restarts the track instead of leaving
	// it: the same rule every player follows, so a double press is what goes
	// back.
	if (state.progress_current_secs > 3.0) {
		device_state_seek(0); // far enough in -> restart current track
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing(); // near the start -> go to previous track
	} else {
		device_state_seek(0); // no queue -> just restart
	}

	update_progress();
}

void player_screen_next(void) {
	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

static void prev_btn_event_cb(lv_event_t *e) {
	(void)e;
	player_screen_prev();
}

static void next_btn_event_cb(lv_event_t *e) {
	(void)e;
	player_screen_next();
}

// --- hardware transport keys ---
//
// They do exactly what the on-screen buttons do, whichever page is showing:
// the player does not have to be open for the keys on the side of the device
// to work.

void player_key_play_pause(void) {
	apply_playback_status(device_state_toggle_play_pause());
}

void player_key_next(void) {
	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

void player_key_prev(void) {
	device_state_t state;
	device_state_get(&state);

	if (state.progress_current_secs > 3.0) {
		device_state_seek(0);
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing();
	} else {
		device_state_seek(0);
	}
	update_progress();
}

// The overflow button: the track menu.
static void more_btn_event_cb(lv_event_t *e) { trackmenu_open(lv_event_get_current_target(e)); }

// Steps to the next playback mode: normal, loop the queue, loop the track,
// shuffle once through, shuffle round and round.
static void repeat_btn_event_cb(lv_event_t *e) {
	(void)e;
	playlist_cycle_mode();
	update_repeat_button();
}

static void progress_slider_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_timer_pause(progress_slider_timer); // the poll must not fight the finger
		if (smooth_timer) {
			lv_timer_pause(smooth_timer); // nor the smoothing, which would drag it back
		}
	}

	if (code == LV_EVENT_RELEASED) {
		lv_timer_resume(progress_slider_timer);

		int value = lv_slider_get_value(progress_slider);

		double seconds = view_seconds(value);

		device_state_seek(seconds);

		lv_timer_resume(progress_slider_timer);
	}

	if (code == LV_EVENT_VALUE_CHANGED) {
		// While dragging, the left clock previews where the knob would land.
		int value = lv_slider_get_value(progress_slider);

		double seconds = view_seconds(value);

		formatDoubleSeconds(seconds, progress_label_text, sizeof(progress_label_text));
		lv_label_set_text(elapsed_label, progress_label_text);

		// And so does the waveform, when that is what is being dragged. The
		// poll is paused for the duration of the press, so without this the
		// bars would stand still under the finger and only jump at the release.
		wave_preview(seconds);
	}
}

// The poll: collects finished artwork and reconciles the UI with playback.
// Carries the bar and the shape of the track forward between polls.
//
// Draws nothing unless the picture would change: the slider is told a value
// only when its own scale says a different one, and the waveform only when the
// playhead has crossed into another pixel. Both are compared against what was
// last drawn rather than against what was last computed, so a tick that changes
// nothing costs nothing.
static void smooth_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!progress_running || view_length <= 0.1) {
		return;
	}
	// Nothing is drawn for a screen nobody is looking at, or for a page that is
	// not the one on show. Drawing a dark panel is work this device cannot
	// spare and has paid for before.
	if (!sheet_open || !power_screen_is_on()) {
		return;
	}

	double seconds = progress_anchor_secs + (double)lv_tick_elaps(progress_anchor_tick) / 1000.0;
	if (seconds > view_length) {
		seconds = view_length; // the poll will say what happens next
	}

	int value = view_value(seconds);
	if (value != lv_slider_get_value(progress_slider)) {
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
	}
	wave_preview(seconds);
	lyrics_follow(seconds);
}

static void progress_slider_timer_cb(lv_timer_t *timer) {
	(void)timer;
	apply_cover_id_result(); // "is the new track's picture the one already up?"
	apply_cover_result();	 // artwork the worker has finished since last tick
	update_progress();
}

// The boot-time restore: the remembered track, paused at its position.
void player_restore_track(const char *filepath, double position) {
	device_state_restore_file(filepath, position);
	player_refresh_now_playing();
}

// The same, with the queue that was saved alongside it.
void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots,
						  int extra_count, const char *filepath, double position) {
	device_state_restore_index(ix, start_index, extra, extra_slots, extra_count, filepath, position);
	player_refresh_now_playing();
}

void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath,
						 double position) {
	device_state_restore_list(list, count, start_index, custom, filepath, position);
	player_refresh_now_playing();
}

// Loads and starts a new file, updating the now-playing info. Also rebuilds
// the folder queue inside device_state, so playback carries on to the
// following tracks when this one finishes.
void player_play_file(const char *filepath) {
	device_state_play_file(filepath); // builds folder queue, loads metadata, starts playback

	player_refresh_now_playing();
}

player_layout_t player_layout_get(void) { return layout_choice; }

bool player_cover_accent(void) { return cover_accent; }

void player_cover_accent_set(bool on) {
	if (on == cover_accent) {
		return;
	}
	cover_accent = on;
	config_set_bool("screen", "cover_accent", on);
	config_save();
	paint_transport_tint();
}

void player_layout_set(player_layout_t layout) {
	if (layout == layout_choice) {
		return;
	}
	layout_choice = layout;
	config_set_int("screen", "player_layout_alt", (int)layout);
	config_save();

	// Not apply_layout() directly: what the setting does is change the answer
	// update_layout() will give, so it is asked again.
	device_state_t state;
	device_state_get(&state);
	update_layout(&state);
}

void player_output_unplugged(const char *what) {
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}
	printf("player: %s unplugged; pausing\n", what ? what : "the output");
	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// A pause and not a stop, and for the same reason as the one above: the point
// of falling asleep to something is finding it where it was in the morning.
static void sleep_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!sleeptimer_expired(SLEEPTIMER_MUSIC) || audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}

	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// The artist line again, after "Use track artist" has been switched. Not a
// whole refresh_now_playing(): nothing else on the page has changed, and that
// would decode the cover and ask for the lyrics all over again.
void player_refresh_artist(void) {
	if (!song_artist_label) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}
	scrolltext_set(song_artist_label, metadata_shown_artist(&state.metadata));
	lyrics_head_sync();
	alt_pills_sync();
}

// Public face of refresh_now_playing, for callers that started playback
// through device_state themselves (the library lists hand over a whole
// queue rather than a single file).
void player_refresh_now_playing(void) {
	refresh_now_playing();

	// Playback may have been started by something that also set the mode --
	// the favourites shuffle button forces shuffle on its way in -- so the
	// repeat/shuffle glyph is refreshed here too, not only when it is tapped.
	update_repeat_button();

	device_state_t state;
	device_state_get(&state);
	apply_playback_status(state.status);
}


// ---------------------------------------------------------------------------
// the sheet: dragging the player in and out
// ---------------------------------------------------------------------------

#define SHEET_ANIM_MS 220

static int sheet_width;
static lv_obj_t *sheet_under; // the page the sheet first slid over

static void sheet_anim_x_cb(void *obj, int32_t value) {
	lv_obj_set_x((lv_obj_t *)obj, (int)value);
	// The chevron belongs to the sheet while it moves: ride its edge, and
	// snap back into the page's corner once the sheet is fully away.
	back_btn_translate(value >= sheet_width ? 0 : value);
}

// Moves the sheet to `x` (0 = fully open, sheet_width = fully out of the way).
static void sheet_move_to(int x, bool animate) {
	if (x < 0) {
		x = 0;
	}
	if (x > sheet_width) {
		x = sheet_width;
	}

	// Never race a slide that is still running. A slide left to finish after a
	// new one has started parks the sheet off screen while everything else
	// believes it is open -- the page underneath showing, no status bar, and no
	// way back to the player.
	lv_anim_delete(player_screen, sheet_anim_x_cb);

	if (!animate) {
		lv_obj_set_x(player_screen, x);
		back_btn_translate(x >= sheet_width ? 0 : x);
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), x);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

bool player_sheet_is_open(void) { return sheet_open; }

// Minimal R1: sideways sheet drag is off (home tile / back only).
bool player_sheet_drag_active(void) { return false; }

// The status bar over the open player, when it stays up there: blurring the
// cover under it, on nothing in Studio -- the blurred picture behind
// everything is already there -- and in the page's own colours while there is
// no picture at all, where white would be lost on a light theme.
static void bar_over_player_update(void) {
	if (!sheet_open || !bar_always) {
		return;
	}
	topbar_set_hidden(false);
	if (!current_cover.pixels) {
		topbar_set_over_artwork(false, false);
		return;
	}
	topbar_set_over_artwork(true, !layout_studio_now);
}

bool player_statusbar_always(void) { return bar_always; }

void player_set_statusbar_always(bool on) {
	if (on == bar_always) {
		return;
	}
	bar_always = on;
	back_btn_player_below_bar(on);

	// Everything lined up with the chevron follows it down or back up.
	if (layout_studio_now) {
		studio_place();
	}
	lyrics_layout();

	if (!on) {
		if (sheet_open) {
			topbar_set_over_artwork(false, false);
			topbar_set_hidden(true);
		}
		return;
	}
	bar_over_player_update();
}

void player_sheet_open(bool animate) {
	(void)animate;
	animate = false; // Minimal R1: instant open, no slide
	// Nothing to show while another mode owns the sound: the sheet is about a
	// track this player is playing, and in Bluetooth receiver mode the track is
	// on somebody else's machine. Refusing to open is the honest answer -- an
	// opened sheet would show the last thing played here, paused, over music
	// that is coming out of the jack right now.
	if (btreceiver_is_active()) {
		return;
	}

	if (!sheet_open) {
		// Remember the opener: leaving the player must always land back here,
		// whatever the pages under the sheet did in the meantime.
		sheet_under = lv_screen_active();
	}
	sheet_open = true;
	smooth_timer_sync();
	back_btn_player_mode(true);
	lyrics_screen_hold_update();

	// Explicitly the bottom-most thing on the top layer: it covers the page it
	// slides over, while the status bar and the floating back button stay drawn
	// above it, exactly where the page underneath put them. Nothing shared is
	// moved or hidden, so nothing shared slides around during the drag.
	lv_obj_set_hidden(player_screen, false);
	lv_obj_move_background(player_screen);

	// Artwork runs to the top edge here, so the bar steps aside. It is hidden
	// rather than moved, which is why nothing slides during the drag. Unless
	// it is to stay up: then it goes over the artwork.
	if (bar_always) {
		bar_over_player_update();
	} else {
		topbar_set_hidden(true);
	}

	sheet_move_to(0, animate);
}

static void sheet_hidden_anim_ready_cb(lv_anim_t *a) {
	(void)a;
	if (!sheet_open) {
		lv_obj_set_hidden(player_screen, true);
	}
	// Whatever page the close landed on, the chevron's visibility is settled
	// once more now that the last animation frame has run -- the fix for the
	// chevron flashing on the main menu for a frame on the way back to it.
	back_btn_sync_visibility();
}

void player_sheet_close(bool animate) {
	(void)animate;
	animate = false; // Minimal R1: instant close, no slide
	bool was_open = sheet_open;
	sheet_open = false;
	smooth_timer_sync();
	back_btn_player_mode(false);
	lyrics_screen_hold_update();
	topbar_set_over_artwork(false, false);
	topbar_set_hidden(false);
	// The tab does not travel with the page: coming back to a player with a
	// panel hanging open is not what leaving it looked like.

	// Back where the player was opened from, always: if anything switched the
	// page underneath while the sheet was up, put the opener back.
	if (was_open && sheet_under && lv_screen_active() != sheet_under) {
		switch_screen_no_history(sheet_under);
	}
	// The page underneath stays loaded while the sheet covers it, so nothing
	// tells it to look at its list again.
	medialist_refresh_visible();

	if (!animate) {
		lv_obj_set_x(player_screen, sheet_width);
		back_btn_translate(0);
		lv_obj_set_hidden(player_screen, true);
		back_btn_sync_visibility();
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), sheet_width);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_hidden_anim_ready_cb);
	lv_anim_start(&a);
}

void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	// Minimal R1: no sideways swipe to open/close Now Playing — home tile only.
	(void)obj;
	(void)opening;
}

bool player_sheet_edge_up_active(void) { return false; }

void player_sheet_attach_edge_up(lv_obj_t *obj, gui_config_t *cfg) {
	// Minimal R1: no upward swipe to open Now Playing.
	(void)obj;
	(void)cfg;
}

// Builds the player sheet: artwork, controls and the poll that drives them.
void player_init(gui_config_t *cfg) {
	// The artwork decodes on the shared worker; make sure it exists before
	// the first track can be started. Safe to call more than once.
	coverloader_start();
	// The next track's picture is not read ahead once the cover side has no
	// use for it.
	readahead_set_picture_known(cover_picture_known);

	lv_obj_add_style(player_screen, &theme_style_screen, 0);

	// The player is a panel parked just off the right edge (see player.h).
	sheet_width = (int)cfg->screen_width;
	lv_obj_set_size(player_screen, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_pad_all(player_screen, 0, 0);
	lv_obj_set_style_border_width(player_screen, 0, 0);
	lv_obj_set_style_radius(player_screen, 0, 0);
	lv_obj_set_scrollable(player_screen, false);
	lv_obj_set_pos(player_screen, sheet_width, 0);
	lv_obj_set_hidden(player_screen, true);


	// Geometry first, because everything else hangs off it: the artwork is a
	// square as wide as the screen, sitting flush at the top (this page hides
	// the status bar), and the controls get whatever height is left. On a panel
	// too short for that, the artwork gives up height rather than the controls.
	int cover_size = (int)cfg->screen_width;
	int menu_height = (int)cfg->screen_height - cover_size;
	if (menu_height < PLAYER_MENU_MIN_HEIGHT) {
		menu_height = PLAYER_MENU_MIN_HEIGHT;
		cover_size = (int)cfg->screen_height - menu_height;
		if (cover_size < 64)
			cover_size = 64;
	}

	cover_box_w = cover_size;
	cover_box_h = cover_size;
	backdrop_w = (int)cfg->screen_width;
	backdrop_h = menu_height;
	backdrop_studio_h = (int)cfg->screen_height;

	// The controls block. Its background is the current track's artwork,
	// flipped and blurred (set per track in refresh_cover); the panel colour is
	// what shows through when there is no artwork.
	player_menu = lv_obj_create(player_screen);
	lv_obj_set_size(player_menu, cfg->screen_width, menu_height);
	lv_obj_align(player_menu, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_add_style(player_menu, &theme_style_panel, 0);
	lv_obj_set_flex_flow(player_menu, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(player_menu, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_border_width(player_menu, 0, 0);
	lv_obj_set_style_radius(player_menu, 0, 0);
	lv_obj_set_style_pad_hor(player_menu, cfg->padding, 0);
	int spare = menu_height - PLAYER_MENU_REF_HEIGHT;
	if (spare > 0) {
		menu_pad_ver = PLAYER_MENU_PAD_VER + spare / 5;
		menu_gap = PLAYER_MENU_GAP + spare / 5;
	}
	lv_obj_set_style_pad_ver(player_menu, menu_pad_ver, 0);
	lv_obj_set_style_pad_gap(player_menu, menu_gap, 0);
	lv_obj_set_scrollable(player_menu, false);

	// Track info: the text on the left, the star and the format on the right.
	lv_obj_t *song_info = lv_obj_create(player_menu);
	lv_obj_set_size(song_info, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_info, 0, 0);
	lv_obj_set_style_border_width(song_info, 0, 0);
	lv_obj_set_style_radius(song_info, 0, 0);
	lv_obj_set_style_pad_all(song_info, 0, 0);
	lv_obj_set_style_pad_column(song_info, 10, 0);
	lv_obj_set_flex_flow(song_info, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(song_info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(song_info, false);
	// Every time the row is laid out (a new text size, the star shown or
	// hidden, the row coming back from another arrangement) the title is lined
	// up with the star again.
	lv_obj_add_event_cb(song_info, song_info_layout_cb, LV_EVENT_LAYOUT_CHANGED, NULL);

	lv_obj_t *song_text = lv_obj_create(song_info);
	song_text_obj = song_text;
	lv_obj_set_height(song_text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(song_text, 1);
	lv_obj_set_style_bg_opa(song_text, 0, 0);
	lv_obj_set_style_border_width(song_text, 0, 0);
	lv_obj_set_style_pad_all(song_text, 0, 0);
	lv_obj_set_flex_flow(song_text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(song_text, false);

	// Long titles scroll rather than end in an ellipsis: on a 480 px panel a
	// good half of real album titles do not fit, and the tail is usually the
	// part that tells them apart.
	song_title_label = lv_label_create(song_text);
	lv_label_set_text(song_title_label, tr("player_no_track_loaded"));
	lv_obj_set_width(song_title_label, lv_pct(100));
	lv_obj_add_style(song_title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(song_title_label, &font_ui_26, 0);
	scrolltext_apply(song_title_label);

	// The artist line -- see refresh_now_playing().
	song_artist_label = lv_label_create(song_text);
	lv_label_set_text(song_artist_label, "");
	lv_obj_set_width(song_artist_label, lv_pct(100));
	lv_obj_add_style(song_artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(song_artist_label, &font_ui_24, 0);
	scrolltext_apply(song_artist_label);

	// Right side: the favourite star over the stream format.
	lv_obj_t *song_side = lv_obj_create(song_info);
	song_side_obj = song_side;
	lv_obj_set_size(song_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_side, 0, 0);
	lv_obj_set_style_border_width(song_side, 0, 0);
	lv_obj_set_style_pad_all(song_side, 0, 0);
	lv_obj_set_style_pad_gap(song_side, 2, 0);
	lv_obj_set_flex_flow(song_side, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(song_side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(song_side, false);

	lv_obj_t *fav_btn = lv_btn_create(song_side);
	fav_btn_obj = fav_btn;
	lv_obj_set_size(fav_btn, FAV_BTN_W, FAV_BTN_H);
	lv_obj_set_style_bg_opa(fav_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(fav_btn, 0, 0);
	lv_obj_set_style_shadow_width(fav_btn, 0, 0);
	lv_obj_set_style_pad_all(fav_btn, 0, 0);
	lv_obj_add_event_cb(fav_btn, fav_btn_event_cb, LV_EVENT_CLICKED, NULL);

	fav_btn_icon = lv_image_create(fav_btn);
	lv_obj_center(fav_btn_icon);
	update_fav_button();

	format_label = lv_label_create(song_side);
	lv_label_set_text(format_label, "");
	lv_obj_add_style(format_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_18, 0);

	// The shape of the track, and the slider laid over it that still does the
	// seeking. Two things in the same place rather than one: drawing a
	// waveform is one job and reading a drag is another, and the slider
	// already knows how to do the second.
	wave_box = lv_obj_create(player_menu);
	lv_obj_remove_style_all(wave_box);
	lv_obj_set_size(wave_box, lv_pct(100), WAVE_HEIGHT);
	lv_obj_set_scrollable(wave_box, false);
	lv_obj_set_hidden(wave_box, true);

	{
		wave_w = (int)cfg->screen_width - 2 * cfg->padding;
		if (wave_w < WAVEFORM_BARS) {
			wave_w = WAVEFORM_BARS;
		}
		wave_buf = malloc((size_t)wave_w * WAVE_HEIGHT * 3);
		if (wave_buf) {
			memset(wave_buf, 0, (size_t)wave_w * WAVE_HEIGHT * 3);
			wave_canvas = lv_canvas_create(wave_box);
			lv_canvas_set_buffer(wave_canvas, wave_buf, wave_w, WAVE_HEIGHT, LV_COLOR_FORMAT_RGB565A8);
			lv_obj_set_size(wave_canvas, wave_w, WAVE_HEIGHT);
			lv_obj_align(wave_canvas, LV_ALIGN_CENTER, 0, 0);
			lv_obj_set_clickable(wave_canvas, false);
		} else {
			fprintf(stderr, "player: no room for the waveform; the alternative layout keeps the bar\n");
		}
	}

	// Playback progress, drawn as described at PROGRESS_TRACK_HEIGHT.
	progress_slider = lv_slider_create(player_menu);
	lv_obj_set_width(progress_slider, lv_pct(100));
	lv_obj_set_height(progress_slider, PROGRESS_TRACK_HEIGHT);
	lv_slider_set_range(progress_slider, 0, 1000);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_slider, lv_color_make(255, 255, 255), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_INDICATOR);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(progress_slider, theme()->accent, LV_PART_KNOB);
	lv_obj_set_style_border_color(progress_slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_border_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(progress_slider, PROGRESS_KNOB_RING, LV_PART_KNOB);
	lv_obj_set_style_pad_all(progress_slider, PROGRESS_KNOB_GROW, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(progress_slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(progress_slider, LV_OPA_40, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(progress_slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(progress_slider, 1, LV_PART_KNOB);

	lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);

	// Polls playback to keep the slider, the clock and the play/pause icon in
	// sync. It runs from here on, slowing down rather than stopping when
	// nothing is playing (see apply_playback_status).
	smooth_timer = lv_timer_create(smooth_timer_cb, SMOOTH_PERIOD_MS, NULL);
	lv_timer_pause(smooth_timer); // the poll starts it when something is playing

	progress_slider_timer = lv_timer_create(progress_slider_timer_cb, POLL_PERIOD_IDLE_MS, NULL);
	audio_set_completion_hook(track_end_hook);
	// Slowed in standby, never stopped: this timer does more than paint --
	// update_progress() calls device_state_take_completion(), the engine that
	// advances the queue when a track ends on its own. Stopping it when the
	// screen goes off (on the grounds that nobody can see the slider) meant no
	// next track ever started with the screen dark, local files included,
	// until the screen came back on and consumed the completion left waiting.
	// The period is still apply_playback_status's to decide (fast while
	// playing, slow when stopped), in the dark too.
	power_slow_in_standby(progress_slider_timer, POLL_PERIOD_PLAYING_MS);

	// The sleep timer's own tick. Slowed in standby and not stopped, because
	// the screen being dark is the normal case for it -- and slowing it costs
	// nothing in accuracy: the countdown is against the clock, so a late tick
	// stops the music late by however late it was, not by a tick.
	sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	lv_timer_pause(sleep_timer); // apply_playback_status starts it when there is a stretch to count
	power_slow_in_standby(sleep_timer, 5000);

	lv_obj_t *below_slider_group = lv_obj_create(player_menu);
	below_slider_obj = below_slider_group;
	lv_obj_set_size(below_slider_group, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(below_slider_group, 0, 0);
	lv_obj_set_style_border_width(below_slider_group, 0, 0);
	lv_obj_set_style_radius(below_slider_group, 0, 0);
	lv_obj_set_style_pad_all(below_slider_group, 0, 0);
	lv_obj_set_scrollable(below_slider_group, false);

	// Elapsed and total time, tucked under the two ends of the bar.
	elapsed_label = lv_label_create(below_slider_group);
	lv_label_set_text(elapsed_label, "0:00");
	lv_obj_add_style(elapsed_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(elapsed_label, &font_ui_22, 0);
	lv_obj_set_align(elapsed_label, LV_ALIGN_LEFT_MID);

	remaining_label = lv_label_create(below_slider_group);
	lv_label_set_text(remaining_label, "0:00");
	lv_obj_add_style(remaining_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(remaining_label, &font_ui_22, 0);
	lv_obj_set_align(remaining_label, LV_ALIGN_RIGHT_MID);

	// Between them, in the middle the two clocks left empty. Dimmer than they
	// are: it is a bearing, not a reading.
	queue_position_label = lv_label_create(below_slider_group);
	lv_label_set_text(queue_position_label, "");
	lv_obj_add_style(queue_position_label, &theme_style_text_dim, 0);
	// A step below the clocks either side of it, as well as dimmer: it is a
	// bearing and they are the reading, and at the same size the three read as
	// one row of equals.
	lv_obj_set_style_text_font(queue_position_label, &font_ui_18, 0);
	lv_obj_set_align(queue_position_label, LV_ALIGN_CENTER);

	// Transport: prev, play/pause, next -- with the repeat toggle parked on
	// the left, out of the way of the transport controls.
	lv_obj_t *player_controls_buttons = lv_obj_create(player_menu);
	controls_row = player_controls_buttons;
	lv_obj_set_size(player_controls_buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(player_controls_buttons, 0, 0);
	lv_obj_set_style_border_width(player_controls_buttons, 0, 0);
	lv_obj_set_style_radius(player_controls_buttons, 0, 0);
	lv_obj_set_style_pad_all(player_controls_buttons, 0, 0);
	lv_obj_set_flex_flow(player_controls_buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(player_controls_buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Lifted a touch off the screen edge.
	lv_obj_set_style_translate_y(player_controls_buttons, -8, 0);

	// The repeat/shuffle button, kept out of the flex row so the transport
	// stays centred on the screen.
	lv_obj_t *repeat_btn = lv_btn_create(player_controls_buttons);
	repeat_btn_obj = repeat_btn;
	lv_obj_set_ignore_layout(repeat_btn, true);
	lv_obj_set_size(repeat_btn, 56, 56);
	lv_obj_set_style_bg_opa(repeat_btn, 0, 0);
	lv_obj_set_style_shadow_width(repeat_btn, 0, 0);
	lv_obj_add_event_cb(repeat_btn, repeat_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(repeat_btn, LV_ALIGN_LEFT_MID, 0, 0);
	// The glyph stays 56 px; the area that takes the tap does not, out to the
	// height of the row and short of the previous button.
	lv_obj_set_ext_click_area(repeat_btn, CORNER_BTN_EXT_CLICK);

	repeat_btn_icon = lv_image_create(repeat_btn);
	lv_obj_center(repeat_btn_icon);
	update_repeat_button(); // the icon for the mode already in force

	// The overflow menu, mirroring the repeat button on the right: queue and
	// details in an iOS-style popover next to the button.
	lv_obj_t *more_btn = lv_btn_create(player_controls_buttons);
	more_btn_obj = more_btn;
	lv_obj_set_ignore_layout(more_btn, true);
	lv_obj_set_size(more_btn, 56, 56);
	lv_obj_set_style_bg_opa(more_btn, 0, 0);
	lv_obj_set_style_shadow_width(more_btn, 0, 0);
	lv_obj_add_event_cb(more_btn, more_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(more_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_ext_click_area(more_btn, CORNER_BTN_EXT_CLICK);

	lv_obj_t *more_icon = lv_image_create(more_btn);
	more_btn_icon = more_icon;
	lv_image_set_src(more_icon, &icon_ellipsis_vertical);
	lv_obj_add_style(more_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(more_icon, lv_color_make(150, 150, 150), 0);
	lv_obj_set_style_image_recolor_opa(more_icon, LV_OPA_COVER, 0);
	lv_obj_center(more_icon);

	// No button chrome on prev/next: they sit on top of the blurred artwork,
	// and plain glyphs read better there than filled boxes.
	lv_obj_t *prev_btn = lv_btn_create(player_controls_buttons);
	prev_btn_obj = prev_btn;
	lv_obj_set_size(prev_btn, 76, 76);
	lv_obj_set_style_bg_opa(prev_btn, 0, 0);
	lv_obj_set_style_shadow_width(prev_btn, 0, 0);
	prev_icon = lv_image_create(prev_btn);
	lv_image_set_src(prev_icon, &icon_skip_back);
	lv_obj_add_style(prev_icon, &theme_style_icon, 0);
	lv_obj_center(prev_icon);

	lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Play/pause: a white disc, with the glyph carrying the colour.
	play_btn = lv_btn_create(player_controls_buttons);
	lv_obj_set_size(play_btn, 84, 84);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, lv_color_white(), 0); // repainted by set_over_cover
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);

	play_btn_icon = lv_image_create(play_btn);
	lv_obj_center(play_btn_icon);
	apply_playback_status(AUDIO_STATUS_STOPPED); // start on the play glyph

	lv_obj_t *next_btn = lv_btn_create(player_controls_buttons);
	next_btn_obj = next_btn;
	lv_obj_set_size(next_btn, 76, 76);
	lv_obj_set_style_bg_opa(next_btn, 0, 0);
	lv_obj_set_style_shadow_width(next_btn, 0, 0);
	next_icon = lv_image_create(next_btn);
	lv_image_set_src(next_icon, &icon_skip_forward);
	lv_obj_add_style(next_icon, &theme_style_icon, 0);
	lv_obj_center(next_icon);

	lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Album art: a square as wide as the screen, flush against the top edge
	// (this page hides the status bar, see switch_screen). The picture is
	// centre-cropped to fill it rather than letterboxed. Sizes were worked out
	// at the top of this function, before the controls were laid out.
	cover_panel = lv_obj_create(player_screen);
	// Dragging the artwork to the right pushes the player back off screen.
	player_sheet_attach_drag(cover_panel, false);
	// Studio stretches the panel down over the top of the controls, where the
	// taller shape of the track can reach: presses there go to the slider.
	lv_obj_set_adv_hittest(cover_panel, true);
	lv_obj_add_event_cb(cover_panel, cover_panel_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
	lv_obj_set_size(cover_panel, cover_size, cover_size);
	lv_obj_align(cover_panel, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(cover_panel, theme()->cover_bg, 0);
	lv_obj_set_style_border_width(cover_panel, 0, 0);
	lv_obj_set_style_radius(cover_panel, 0, 0);
	lv_obj_set_style_pad_all(cover_panel, 0, 0);
	lv_obj_set_scrollable(cover_panel, false);

	// Shown while there is no artwork for the current track.
	cover_placeholder_icon = lv_image_create(cover_panel);
	lv_image_set_src(cover_placeholder_icon, &icon_music_note);
	lv_obj_add_style(cover_placeholder_icon, &theme_style_text_dim, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder_icon, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder_icon);

	// Where the title and the star go in the alternative layout: two pills in
	// one bottom corner of the sleeve and a disc in the other. Built empty and
	// hidden; apply_layout() moves the real labels into them.
	//
	// Children of cover_panel so they sit on the sleeve and travel with it.
	// The column is what keeps the two pills stacked and both left-aligned
	// while each is only as wide as its own words.
	alt_text_col = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(alt_text_col);
	lv_obj_set_size(alt_text_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(alt_text_col, LV_ALIGN_BOTTOM_LEFT, ALT_PAD, -ALT_PAD);
	lv_obj_set_flex_flow(alt_text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(alt_text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(alt_text_col, 6, 0);
	lv_obj_set_scrollable(alt_text_col, false);
	lv_obj_set_hidden(alt_text_col, true);
	// None of this takes presses. A plain object is clickable the moment it is
	// made, and these lie on the artwork -- which is the surface the player is
	// dragged shut by. Left clickable they swallowed every gesture that began
	// on the title, so putting the player away by pushing it off the right of
	// the screen worked everywhere except on the words naming the track.
	lv_obj_set_clickable(alt_text_col, false);

	for (int i = 0; i < 2; i++) {
		lv_obj_t *pill = lv_obj_create(alt_text_col);
		lv_obj_remove_style_all(pill);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(pill, ALT_PILL_PAD_H, 0);
		lv_obj_set_style_pad_ver(pill, ALT_PILL_PAD_V, 0);
		lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		// No shadow. There was one, to keep a pill from disappearing into a
		// patch of sleeve the same colour as itself -- but that was when the
		// fill was the sleeve's colour at full strength. It is now a dark
		// surface of fixed weight, which separates from artwork on its own, and
		// the shadow under it only made the pill look pasted on.
		lv_obj_set_scrollable(pill, false);
		lv_obj_set_clickable(pill, false);
		if (i == 0) {
			alt_title_pill = pill;
		} else {
			alt_artist_pill = pill;
		}
	}

	alt_fav_circle = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(alt_fav_circle);
	lv_obj_set_size(alt_fav_circle, ALT_FAV_SIZE, ALT_FAV_SIZE);
	lv_obj_set_style_radius(alt_fav_circle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(alt_fav_circle, LV_OPA_COVER, 0);
	lv_obj_align(alt_fav_circle, LV_ALIGN_BOTTOM_RIGHT, -ALT_PAD, -ALT_PAD);
	lv_obj_set_scrollable(alt_fav_circle, false);
	// Likewise: the star's own button is a child of this and takes its own
	// presses, so the disc around it has no reason to take any.
	lv_obj_set_clickable(alt_fav_circle, false);
	lv_obj_set_hidden(alt_fav_circle, true);

	cover_img = lv_image_create(cover_panel);
	lv_obj_center(cover_img);
	lv_obj_set_hidden(cover_img, true);

	// ---------------------------------------------------------------------
	// Studio: the blurred sleeve behind the whole screen, and everything else
	// laid out over it.
	//
	// The background is a child of the page rather than of the artwork, so it
	// reaches under the controls as well; the panel that carries the rest is a
	// child of the artwork, so it travels with the sheet. Nothing on either
	// takes a press: the surface under them is what the player is pushed shut
	// by, and the buttons that do take presses are moved here from elsewhere.
	// ---------------------------------------------------------------------
	studio_bg = lv_image_create(player_screen);
	lv_obj_set_ignore_layout(studio_bg, true);
	lv_obj_set_size(studio_bg, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(studio_bg, 0, 0);
	// COVER and not STRETCH: the blurred copy is asked for at the shape of the
	// screen, but between a change of arrangement and the picture that follows
	// it the one on hand is still the old shape, and stretching that is a
	// visibly squashed sleeve.
	lv_image_set_inner_align(studio_bg, LV_IMAGE_ALIGN_COVER);
	lv_obj_set_clickable(studio_bg, false);
	lv_obj_set_hidden(studio_bg, true);
	lv_obj_move_background(studio_bg);

	studio_box_w = cover_size;
	studio_box_h = (int)cfg->screen_height - STUDIO_CONTROLS_H - 2 * (menu_gap - PLAYER_MENU_GAP);
	if (studio_box_h < cover_size) {
		studio_box_h = cover_size;
	}
	studio_box = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(studio_box);
	lv_obj_set_size(studio_box, studio_box_w, studio_box_h);
	lv_obj_set_pos(studio_box, 0, 0);
	lv_obj_set_scrollable(studio_box, false);
	lv_obj_set_clickable(studio_box, false);
	lv_obj_set_hidden(studio_box, true);

	studio_head = lv_obj_create(studio_box);
	lv_obj_remove_style_all(studio_head);
	lv_obj_set_size(studio_head, cover_size - 2 * STUDIO_MARGIN, STUDIO_HEAD_H);
	lv_obj_set_scrollable(studio_head, false);
	lv_obj_set_clickable(studio_head, false);

	// Centred in the head and not filling it: the ellipsis sits at the right
	// edge and the chevron at the left, and the two names have to stop before
	// they reach either -- on both sides, or a long title would be centred
	// against one of them.
	studio_text_col = lv_obj_create(studio_head);
	lv_obj_remove_style_all(studio_text_col);
	lv_obj_set_size(studio_text_col, cover_size - 2 * STUDIO_MARGIN - 2 * 68, LV_SIZE_CONTENT);
	lv_obj_align(studio_text_col, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(studio_text_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(studio_text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(studio_text_col, 4, 0);
	lv_obj_set_style_text_align(studio_text_col, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_scrollable(studio_text_col, false);
	lv_obj_set_clickable(studio_text_col, false);

	// The sleeve at whatever size studio_place() settles on. The picture is
	// decoded at the width of the screen for the standard arrangement, so it is
	// scaled down here rather than decoded twice.
	studio_cover = lv_image_create(studio_box);
	lv_obj_set_ignore_layout(studio_cover, true);
	lv_image_set_inner_align(studio_cover, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_clickable(studio_cover, false);

	// What stands in the sleeve's place when the track has no artwork: the same
	// surface and the same mark the full-width cover panel uses, at the size
	// this arrangement gives the sleeve.
	studio_empty = lv_obj_create(studio_box);
	lv_obj_set_ignore_layout(studio_empty, true);
	lv_obj_add_style(studio_empty, &theme_style_panel, 0);
	lv_obj_set_style_border_width(studio_empty, 0, 0);
	lv_obj_set_style_radius(studio_empty, 8, 0);
	lv_obj_set_scrollable(studio_empty, false);
	lv_obj_set_clickable(studio_empty, false);
	lv_obj_set_hidden(studio_empty, true);

	studio_empty_icon = lv_image_create(studio_empty);
	lv_obj_set_style_image_recolor_opa(studio_empty_icon, LV_OPA_COVER, 0);
	lv_obj_center(studio_empty_icon);
	lv_obj_set_clickable(studio_empty_icon, false);

	studio_quality = lv_obj_create(studio_box);
	lv_obj_remove_style_all(studio_quality);
	lv_obj_set_flex_flow(studio_quality, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(studio_quality, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(studio_quality, 8, 0);
	lv_obj_set_scrollable(studio_quality, false);
	lv_obj_set_clickable(studio_quality, false);

	// No theme_style_icon here: the four quality marks carry their own colours,
	// which is how the track lists draw them, and a recolour to the text colour
	// flattens all four into the same grey glyph.
	studio_quality_icon = lv_image_create(studio_quality);

	lyrics_build();

	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);

	// The status bar over the player, as it was left.
	bar_height = (int)cfg->top_bar_height;
	player_set_statusbar_always(config_get_int("screen", "statusbar_always", 0) != 0);

	// Whichever arrangement was left selected, now that every widget it moves
	// exists.
	int saved = (int)config_get_int("screen", "player_layout_alt", 0);
	layout_choice = (saved == (int)PLAYER_LAYOUT_ALTERNATIVE)	? PLAYER_LAYOUT_ALTERNATIVE
					: (saved == (int)PLAYER_LAYOUT_STUDIO)		? PLAYER_LAYOUT_STUDIO
																: PLAYER_LAYOUT_STANDARD;
	cover_accent = false; // Minimal R1: greyscale chrome only
	layout_alt_now = false; // apply_layout() below puts the standard one up first
	layout_studio_now = false;
	apply_layout();

	theme_register_refresh(player_refresh_theme);
}


