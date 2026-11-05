#include "jpdict.h"

#include <string.h>

/* Minimal R1: Japanese kanji dictionary dropped. Romaji→kana still works. */

bool jpdict_lookup(const char *reading, jpdict_hit_t *hit) {
	(void)reading;
	if (hit) {
		memset(hit, 0, sizeof(*hit));
	}
	return false;
}

size_t jpdict_candidate(const jpdict_hit_t *hit, int index, char *out, size_t cap) {
	(void)hit;
	(void)index;
	if (out && cap) {
		out[0] = '\0';
	}
	return 0;
}

bool jpdict_lookup_before(const char *text, unsigned cursor, unsigned max_chars, jpdict_hit_t *hit, char *reading, size_t cap,
						  unsigned *chars) {
	(void)text;
	(void)cursor;
	(void)max_chars;
	if (hit) {
		memset(hit, 0, sizeof(*hit));
	}
	if (reading && cap) {
		reading[0] = '\0';
	}
	if (chars) {
		*chars = 0;
	}
	return false;
}
