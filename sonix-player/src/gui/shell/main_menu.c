#include "main_menu.h"

#include <stdint.h>

#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/switcher.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"

#include "lvgl/lvgl.h"

lv_obj_t *main_menu_screen;

static void open_artists(void) {
	medialist_open(tr("artists"), LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL);
}

static void open_genres(void) {
	medialist_open(tr("music_genres"), LIBRARY_LIST_GENRES, LIBRARY_FILTER_NONE, NULL);
}

static void open_now_playing(void) { player_sheet_open(false); /* no slide animation */ }

void main_menu_init(gui_config_t *cfg) {
	/* Flat home 2×2, text-only uppercase literals (no tr keys).
	 * BLUETOOTH is two lines so it fits a half-width tile at the same size
	 * as NOW PLAYING (one long word overflows; a space would look truncated). */
	const grid_entry_t entries[] = {
		{"NOW\nPLAYING", NULL, NULL, open_now_playing},
		{"BLUE\nTOOTH", NULL, &btsettings_screen, NULL},
		{"ARTISTS", NULL, NULL, open_artists},
		{"GENRES", NULL, NULL, open_genres},
	};

	gridpage_build(main_menu_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 2, false);
}
