#pragma once
#include "lvgl/lvgl.h"
extern lv_obj_t *podcastcard_feed_screen;
extern lv_obj_t *qobuz_list_screen;
extern lv_obj_t *tidal_list_screen;
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define AUDIOBOOK_SKIP_SHORT 10
#define AUDIOBOOK_SKIP_LONG 30
#define AUDIOBOOK_SKIP_HUGE 60
#define AUDIOBOOK_SPEED_NORMAL 1000
#define QOBUZSYNC_PLAYLISTS_MAX 1
#define TIDALSYNC_PLAYLISTS_MAX 1
typedef struct { long long id; char title[128]; char author[128]; char image[512]; } podcast_feed_t;
typedef struct { char uuid[40]; char name[128]; char codec[16]; int bitrate; int bits; int sample_rate; } radio_station_t;
typedef struct { char title[128]; char artist[128]; char codec[16]; int bitrate; int bits; int sample_rate; } radio_now_t;
typedef struct { long id; char name[128]; int track_count; } qobuz_playlist_t;
typedef struct { char id[64]; char name[128]; int track_count; } tidal_playlist_t;
typedef void (*qobuzsync_done_cb)(bool ok, const char *error, void *user);
typedef void (*tidalsync_done_cb)(bool ok, const char *error, void *user);
static inline int audiobook_skip_back(void) { return 15; }
static inline int audiobook_skip_forward(void) { return 30; }
static inline double audiobook_speed(void) { return 1.0; }
static inline int audiobook_speed_permille(void) { return AUDIOBOOK_SPEED_NORMAL; }
static inline void audiobook_set_speed_permille(int p) { (void)p; }
static inline bool audiobook_stop_at_chapter_end(void) { return false; }
static inline bool audiobook_duration_per_chapter(void) { return false; }
static inline void audiobook_suppress_rewind_once(void) {}
static inline bool audiobook_is_playing(void) { return false; }
static inline int audiobook_chapter_count(void) { return 0; }
static inline int audiobook_part_count(void) { return 0; }
static inline int audiobook_chapter_at(double s) { (void)s; return -1; }
static inline int audiobook_part_current(void) { return -1; }
static inline bool audiobook_chapter(int i, char *t, size_t ts, double *start) { (void)i;(void)t;(void)ts; if(start)*start=0; return false; }
static inline bool audiobook_part(int i, char *t, size_t ts, char *p, size_t ps) { (void)i;(void)t;(void)ts;(void)p;(void)ps; return false; }
static inline void audiobook_note_position(double a, double b, bool c) { (void)a;(void)b;(void)c; }
static inline void audiobook_track_changed(const char *p) { (void)p; }
static inline int podcast_skip_back(void) { return 15; }
static inline int podcast_skip_forward(void) { return 30; }
static inline int podcast_speed_permille(void) { return 1000; }
static inline void podcast_set_speed_permille(int p) { (void)p; }
static inline bool podcast_stop_at_episode_end(void) { return false; }
static inline bool podcastcache_is_episode(const char *p) { (void)p; return false; }
static inline bool podcastcache_feed_id(const char *p, long long *id) { (void)p; if(id)*id=0; return false; }
static inline bool podcastcache_tag(const char *path, const char *key, char *out, size_t n) { (void)path;(void)key; if(out&&n)out[0]=0; return false; }
static inline bool podcastcache_ensure_cover(const char *path) { (void)path; return false; }
static inline bool podcastcache_album_id(const char *p, char *o, size_t n) { (void)p; if(o&&n)o[0]=0; return false; }
static inline bool podcastsubs_is_followed(long long id) { (void)id; return false; }
static inline bool podcastsubs_follow(long long id) { (void)id; return false; }
static inline bool podcastsubs_unfollow(long long id) { (void)id; return false; }
static inline bool podcastsubs_get(long long id, podcast_feed_t *f) { (void)id; if(f) memset(f,0,sizeof(*f)); return false; }
static inline bool podcastpage_open_feed(long long id, const char *t) { (void)id;(void)t; return false; }
static inline bool qobuzpage_open_album(const char *id, const char *t) { (void)id;(void)t; return false; }
static inline bool tidalpage_open_album(const char *id, const char *t) { (void)id;(void)t; return false; }
static inline bool radio_is_connecting(void) { return false; }
static inline bool radio_is_active(void) { return false; }
static inline bool radio_can_step(void) { return false; }
static inline bool radio_list_position(int *i, int *t) { if(i)*i=0; if(t)*t=0; return false; }
static inline bool radio_current_station(radio_station_t *s) { if(s) memset(s,0,sizeof(*s)); return false; }
static inline bool radio_get_now(radio_now_t *n) { if(n) memset(n,0,sizeof(*n)); return false; }
static inline unsigned radio_now_serial(void) { return 0; }
static inline void radio_store_open(const char *root) { (void)root; }
static inline bool gearboy_running(void) { return false; }
static inline bool qobuzcache_owns(const char *p) { (void)p; return false; }
static inline bool tidalcache_owns(const char *p) { (void)p; return false; }
static inline long qobuzcache_track_id(const char *p) { (void)p; return 0; }
static inline long tidalcache_track_id(const char *p) { (void)p; return 0; }
static inline bool qobuzcache_album_id(const char *p, char *o, size_t n) { (void)p; if(o&&n)o[0]=0; return false; }
static inline bool tidalcache_album_id(const char *p, char *o, size_t n) { (void)p; if(o&&n)o[0]=0; return false; }
static inline bool qobuzsync_is_favorite(long id) { (void)id; return false; }
static inline bool tidalsync_is_favorite(long id) { (void)id; return false; }
static inline bool qobuzsync_favorites_known(void) { return false; }
static inline bool tidalsync_favorites_known(void) { return false; }
static inline void qobuzsync_favorite_toggle(long id, bool w, qobuzsync_done_cb cb, void *u) { (void)id;(void)w; if(cb) cb(false,"removed",u); }
static inline void tidalsync_favorite_toggle(long id, bool w, tidalsync_done_cb cb, void *u) { (void)id;(void)w; if(cb) cb(false,"removed",u); }
static inline void qobuzsync_favorites_refresh(void (*d)(void)) { (void)d; }
static inline void tidalsync_favorites_refresh(void (*d)(void)) { (void)d; }
static inline void qobuzsync_playlist_add(long a, long b, qobuzsync_done_cb cb, void *u) { (void)a;(void)b; if(cb) cb(false,"removed",u); }
static inline void tidalsync_playlist_add(const char *a, long b, tidalsync_done_cb cb, void *u) { (void)a;(void)b; if(cb) cb(false,"removed",u); }
static inline void qobuzsync_playlist_create_with(const char *n, long t, qobuzsync_done_cb cb, void *u) { (void)n;(void)t; if(cb) cb(false,"removed",u); }
static inline void tidalsync_playlist_create_with(const char *n, long t, tidalsync_done_cb cb, void *u) { (void)n;(void)t; if(cb) cb(false,"removed",u); }
static inline int qobuzsync_playlists(qobuz_playlist_t *o, int m) { (void)o;(void)m; return 0; }
static inline int tidalsync_playlists(tidal_playlist_t *o, int m) { (void)o;(void)m; return 0; }
static inline bool qobuzsync_playlists_known(void) { return true; }
static inline bool tidalsync_playlists_known(void) { return true; }
static inline void qobuzsync_playlists_refresh(void (*d)(void)) { (void)d; }
static inline void tidalsync_playlists_refresh(void (*d)(void)) { (void)d; }
static inline void qobuz_init(void) {}
static inline void tidal_init(void) {}
static inline void lastfm_init(void) {}
static inline void audiobookextras_add_bookmark(void) {}
static inline void audiobookextras_open_current_bookmarks(void) {}
static inline void audiobookextras_open_current_summary(void) {}
static inline void audiobookextras_init(void *cfg) { (void)cfg; }
static inline bool lyrics_offer_wifi(void) { return false; }
static inline bool audiobookdb_open(const char *p) { (void)p; return false; }
static inline void audiobookdb_close(void) {}
static inline int audiobookdb_count(void) { return 0; }
