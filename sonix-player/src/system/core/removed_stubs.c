#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/net/http.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/remote/sonixlink.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/settings/ccsettings.h"
#include "src/gui/settings/lastfmsettings.h"
#include "lvgl/lvgl.h"

lv_obj_t *ccsettings_screen;
lv_obj_t *lastfmsettings_screen;
lv_obj_t *podcastcard_feed_screen;
lv_obj_t *qobuz_list_screen;
lv_obj_t *tidal_list_screen;
lv_obj_t *audiobooks_screen;
lv_obj_t *audiobookscan_screen;

void wifi_init(void) {}
bool wifi_available(void) { return false; }
bool wifi_get_enabled(void) { return false; }
void wifi_set_enabled(bool on) { (void)on; }
void wifi_set_enabled_transient(bool on) { (void)on; }
void wifi_get_status(wifi_status_t *out) { if (out) memset(out, 0, sizeof(*out)); }
bool wifi_interface_address(char *out, size_t size) { if (out && size) out[0] = 0; return false; }
void wifi_set_status_poll_slow(bool slow) { (void)slow; }
void wifi_scan_start(void) {}
bool wifi_scan_running(void) { return false; }
int wifi_get_networks(wifi_network_t *out, int max) { (void)out; (void)max; return 0; }
void wifi_connect(const char *ssid, const char *psk) { (void)ssid; (void)psk; }
void wifi_disconnect(void) {}
void wifi_forget(const char *ssid) { (void)ssid; }
bool wifi_busy(void) { return false; }
void wifitransfer_init(void) {}
bool wifitransfer_is_active(void) { return false; }
void wifitransfer_stop(void) {}
bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs) {
	(void)url; (void)out; (void)out_len; (void)limit; (void)timeout_secs; return false;
}
bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)url; (void)extra_headers; (void)want_error_body; (void)out; (void)out_len; (void)limit; (void)timeout_secs;
	if (status_out) *status_out = 0; return false;
}
void http_url_encode(const char *in, char *out, size_t out_sz) { if (!out || !out_sz) return; out[0]=0; (void)in; }
const char *http_last_error(void) { return "removed"; }
void airplay_init(void) {}
void airplay_get_state(airplay_state_t *out) { if (out) memset(out, 0, sizeof(*out)); }
bool airplay_is_active(void) { return false; }
void dlna_init(void) {}
void dlna_set_root(const char *root) { (void)root; }
bool dlna_owns_playback(void) { return false; }
bool dlna_owns_path(const char *path) { (void)path; return false; }
void dlna_notify_finished(void) {}
void dlna_request_next(void) {}
void dlna_request_prev(void) {}
void sonixlink_init(void) {}
void sonixlink_set_db_path(const char *path) { (void)path; }
void sonixlink_set_thumbs_path(const char *path) { (void)path; }
bool gearboy_running(void) { return false; }
void gearboy_stop(void) {}
void quickpanel_init(gui_config_t *cfg) { (void)cfg; }
void quickpanel_open(void) {}
void quickpanel_close(void) {}
bool quickpanel_is_open(void) { return false; }
bool quickpanel_blocked(void) { return false; }
void quickpanel_drag_begin(void) {}
void quickpanel_drag_update(int dy) { (void)dy; }
void quickpanel_drag_end(void) {}
void ccsettings_init(gui_config_t *cfg) { (void)cfg; }
void lastfmsettings_init(gui_config_t *cfg) { (void)cfg; }
