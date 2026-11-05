#pragma once
#include "src/gui/shell/gui.h"
#include <stdbool.h>
void quickpanel_init(gui_config_t *cfg);
void quickpanel_open(void);
void quickpanel_close(void);
bool quickpanel_is_open(void);
bool quickpanel_blocked(void);
void quickpanel_drag_begin(void);
void quickpanel_drag_update(int dy);
void quickpanel_drag_end(void);
