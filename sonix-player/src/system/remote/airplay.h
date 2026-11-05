#pragma once
#include <stdbool.h>
typedef struct { bool active; bool playing; char title[128]; char artist[128]; } airplay_state_t;
void airplay_init(void);
void airplay_get_state(airplay_state_t *out);
bool airplay_is_active(void);
