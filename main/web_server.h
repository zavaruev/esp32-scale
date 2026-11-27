#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/* Brings up WiFi STA and the HTTP server. Returns immediately; the connection
 * runs in the background and retries on its own. */
void web_weight_json(char *buf, size_t cap);

esp_err_t net_start(void);

/* Scans and prints nearby APs with their auth mode. The host has no WiFi radio, so
 * this is the only way to tell whether "home4" is WPA3-only, hidden, or on a
 * band ESP32 cannot use. */
void wifi_scan_report(void);

/* Fetches our own page over loopback and prints the status and size. */
void web_selftest(void);

/* Brings up an open AP named ESP32-Scale at 192.168.4.1 if STA has not
 * connected within the timeout. Called once from app_main. */
void wifi_start_ap_fallback(void);

/* Leaves the fallback soft AP and retries the home network. */
void wifi_leave_ap_mode(void);

/* Sets the transmit power limit in dBm and verifies it took effect. Returns the
 * value the driver reports, or INT8_MIN if the call failed. */
int wifi_set_tx_power(int dbm);

/* Stops (true) or restarts (false) the WiFi radio, for noise-floor measurements
 * that must be taken with the transmitter silent. */
void wifi_suspend(bool off);

/* 0 = WPA2, 1 = WPA3-SAE, 2 = WPA2/WPA3 transition. */
void wifi_set_auth_mode(int mode);

/* Toggles Protected Management Frames and reconnects. */
void wifi_set_pmf(bool on);

/* Locks the station to one access point by MAC, or "off" to clear. */
void wifi_set_bssid(const char *txt);

/* Raises the WiFi driver log level to DEBUG (true) or back to WARN (false).
 *
 * reason=2 is WIFI_REASON_AUTH_EXPIRE - the AP dropping the station during
 * authentication - which says nothing about why. At DEBUG the driver and the
 * supplicant print the real status codes, so this is the difference between
 * guessing at the cause and being told. */
void wifi_set_debug_log(bool on);

/* Sets the station MAC address before connecting, or "off" to restore the
 * hardware one. Must be called before esp_wifi_start() to take effect. */
void wifi_set_mac(const char *txt);

/* Overrides the password at runtime; pass NULL to restore the built-in one. */
void wifi_set_password(const char *pw);
int wifi_get_auth_mode(void);

int16_t wifi_get_rssi(void);

void ntp_start(void);

#endif /* WEB_SERVER_H */