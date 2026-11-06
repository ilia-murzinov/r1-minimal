#ifndef LYRICS_H
#define LYRICS_H

#include <stdbool.h>
#include <stdint.h>

// The words of a track, for the player's lyrics view.
//
// Looked for in this order, the first found wins:
//
//   1. beside the track: "<file name>.lrc", then "<title>.lrc";
//   2. card-root Lyrics/: same names, then "<artist> - <title>.lrc";
//   3. the lyrics the file carries (metadata_read_lyrics());
//   4. beside the track: "<file name>.txt", then "<title>.txt";
//   5. Lyrics/: same names, then "<artist> - <title>.txt".
//
// The text may be LRC, with [mm:ss.xx] stamps and an [offset:] tag, in which
// case the lines are timed and sorted by time; otherwise every line is kept as
// it is, untimed. A file on the card may be UTF-8 (with or without a BOM),
// UTF-16 with a BOM, or Latin-1.

typedef struct {
	int32_t ms;		  // when the line is sung, -1 when the lyrics are not timed
	const char *text; // may be empty: a pause between verses
} lyrics_line_t;

typedef struct {
	lyrics_line_t *lines;
	int count;
	bool synced;
	char *arena; // the text every line points into
} lyrics_t;

// Reads the lyrics of `path`. `title` / `artist` name sidecar files (beside
// the track and under <sd>/Lyrics/); NULL or empty skips that form. False,
// with `out` empty, when there are none. Reads the card: not on the interface
// thread.
bool lyrics_load(const char *path, const char *title, const char *artist, lyrics_t *out);

void lyrics_free(lyrics_t *lyrics);

// The line being sung at `ms` into the track: the last one whose time has
// come. -1 before the first line, and for untimed lyrics.
int lyrics_line_at(const lyrics_t *lyrics, int32_t ms);

#endif /* LYRICS_H */
