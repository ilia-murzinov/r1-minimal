#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef enum { WIFI_STATE_OFF=0, WIFI_STATE_DISCONNECTED, WIFI_STATE_CONNECTING, WIFI_STATE_CONNECTED } wifi_state_t;
typedef struct { wifi_state_t state; char ip[16]; char ssid[64]; } wifi_status_t;
typedef struct { char ssid[64]; int signal; bool known; } wifi_network_t;
void wifi_init(void);
bool wifi_available(void);
bool wifi_get_enabled(void);
void wifi_set_enabled(bool on);
void wifi_set_enabled_transient(bool on);
void wifi_get_status(wifi_status_t *out);
bool wifi_interface_address(char *out, size_t size);
void wifi_set_status_poll_slow(bool slow);
void wifi_scan_start(void);
bool wifi_scan_running(void);
int wifi_get_networks(wifi_network_t *out, int max);
void wifi_connect(const char *ssid, const char *psk);
void wifi_disconnect(void);
void wifi_forget(const char *ssid);
bool wifi_busy(void);
