#include "main_menu.h"

#include <stdint.h>

#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/switcher.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"

#include "lvgl/lvgl.h"

lv_obj_t *main_menu_screen;

// Minimal home has no Settings → Library path. Empty DB: tap Artists/Genres
// opens the scan flow; long-press either tile rescans anytime.
static void open_library_scan(void) { libraryscan_choose_folders(); }

static void open_artists(void) {
	if (library_track_count() == 0) {
		open_library_scan();
		return;
	}
	medialist_open(tr("artists"), LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL);
}

static void open_genres(void) {
	if (library_track_count() == 0) {
		open_library_scan();
		return;
	}
	medialist_open(tr("music_genres"), LIBRARY_LIST_GENRES, LIBRARY_FILTER_NONE, NULL);
}

static void open_now_playing(void) { player_sheet_open(false); /* no slide animation */ }

static void rescan_long_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || player_sheet_edge_up_active() || switcher_back_drag_active()) {
		return;
	}
	(void)e;
	// LVGL still fires CLICKED on the lift after LONG_PRESSED; wait_release
	// eats that so Artists/Genres do not open under the folder picker.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}
	open_library_scan();
}

void main_menu_init(gui_config_t *cfg) {
	/* Flat home 2×2, text-only uppercase literals (no tr keys).
	 * SETTINGS opens the Bluetooth page (retitled) plus USB/ADB. */
	const grid_entry_t entries[] = {
		{"NOW\nPLAYING", NULL, NULL, open_now_playing},
		{"SETTINGS", NULL, &btsettings_screen, NULL},
		{"ARTISTS", NULL, NULL, open_artists},
		{"GENRES", NULL, NULL, open_genres},
	};

	lv_obj_t *grid = gridpage_build(main_menu_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 2,
									false);

	// Long-press Artists or Genres → rescan (no Settings on this build).
	for (int i = 2; i <= 3; i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, i);
		if (tile) {
			lv_obj_add_event_cb(tile, rescan_long_cb, LV_EVENT_LONG_PRESSED, NULL);
		}
	}
}
