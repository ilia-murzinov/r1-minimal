#pragma once
#include <stddef.h>
#include <stdbool.h>
bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs);
bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len, size_t limit, int timeout_secs, int *status_out);
void http_url_encode(const char *in, char *out, size_t out_sz);
const char *http_last_error(void);
