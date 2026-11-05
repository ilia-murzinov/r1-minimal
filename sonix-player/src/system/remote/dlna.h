#pragma once
#include <stdbool.h>
void dlna_init(void);
void dlna_set_root(const char *root);
bool dlna_owns_playback(void);
bool dlna_owns_path(const char *path);
void dlna_notify_finished(void);
void dlna_request_next(void);
void dlna_request_prev(void);
