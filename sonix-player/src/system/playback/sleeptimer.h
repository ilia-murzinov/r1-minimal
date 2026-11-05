#ifndef SLEEPTIMER_H
#define SLEEPTIMER_H

#include <stdbool.h>

// Sleep timer for local music. Counts down against the monotonic clock while
// something is playing; pausing and starting again gives a full stretch back.
typedef enum {
	SLEEPTIMER_MUSIC = 0,
	SLEEPTIMER_COUNT,
} sleeptimer_kind_t;

void sleeptimer_init(void);

bool sleeptimer_enabled(sleeptimer_kind_t kind);
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on);

int sleeptimer_minutes(sleeptimer_kind_t kind);
void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes);

// Which kind is playing, or SLEEPTIMER_COUNT for none.
void sleeptimer_note_playing(sleeptimer_kind_t kind);

bool sleeptimer_running(sleeptimer_kind_t kind);
bool sleeptimer_any_running(void);

// True exactly once on the tick that kind's time runs out.
bool sleeptimer_expired(sleeptimer_kind_t kind);

int sleeptimer_remaining(sleeptimer_kind_t kind);

#endif /* SLEEPTIMER_H */
