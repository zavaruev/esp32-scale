/*
 * web_server.c - WiFi STA + HTTP server + the scale UI.
 *
 * The UI follows DESIGN.md, which is an extracted Apple design system. The
 * tokens below are copied verbatim from that file so the page and the spec
 * cannot drift apart silently.
 *
 *   primary #0066cc, focus #0071e3, on-dark #2997ff
 *   canvas #ffffff, parchment #f5f5f7, tile #272729
 *   ink #1d1d1f, muted-80 #333333, muted-48 #7a7a7a, hairline #e0e0e0
 *   radii: pill 9999, sm 8, lg 18
 *   spacing base 8; button padding 11px/22px; active scale(0.95)
 *
 * DESIGN.md is explicit that the ONLY drop shadow in the system belongs to a
 * product render resting on a surface, so the weight readout gets it and the
 * cards do not.
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "credentials.h"
#include "index_html.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "scale_filter.h"
#include "weight_task.h"

static const char *TAG = "web";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DRIVER_READY_BIT BIT1
static EventGroupHandle_t wifi_events;
static volatile int wifi_retry_ms = 0;

/* Gate on the WIFI_EVENT_STA_START handler so the driver stays idle between
 * set_mode() and set_config(); otherwise it auto-connects with an empty config. */
static volatile bool wifi_config_want_connect = true;

/* Once the station has held an IP from home4, the soft AP is never needed again.
 * Re-entering it would force another driver restart, and the device would be
 * unreachable on the home network every time it happened. */
static bool sta_ever_connected;

/* Set while a scan is in progress. The reconnect handler must stand down for the
 * duration, otherwise the station keeps re-entering auth and the scan is
 * refused with ESP_ERR_WIFI_STATE. */
static volatile bool wifi_scanning;

/* Selectable at runtime: an AP that advertises WPA2-PSK can still reject a
 * WPA2 client (mis-advertised transition mode, strict PMF policy, SAE-only
 * group policy). Reflashing to test each variant is wasteful, so the mode is
 * switchable over the console. */
typedef enum {
    AUTH_MODE_WPA2 = 0,
    AUTH_MODE_WPA3_SAE,
    AUTH_MODE_WPA2_WPA3,
} auth_mode_sel_t;

static auth_mode_sel_t auth_mode = AUTH_MODE_WPA2;

/* PMF is toggled at runtime because the router UI offers 802.11w = Optional and
 * warns that some client drivers mishandle it. Every earlier PMF test was
 * meaningless: the password was being rejected before it reached the chip. */
static bool pmf_capable = true;

/* Locking onto a specific BSSID.
 *
 * The scan shows home4 twice: a 5 GHz segment at -69 and the 2.4 GHz one that
 * ESP32-S3 can actually use at -76. When one AP advertises the same SSID on
 * several radios the STA just picks whichever it hears best, and there is no
 * firmware-side way to prefer the other one. Pinning the BSSID is the only
 * direct test, and it also helps when a router refuses clients on one band. */
static uint8_t bssid_lock[6];
static bool bssid_locked;

static bool mac_from_text(const char *txt, uint8_t *out)
{
    unsigned v[6];
    if (sscanf(txt, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
    }
    return true;
}

void wifi_set_debug_log(bool on)
{
    esp_log_level_t lvl = on ? ESP_LOG_DEBUG : ESP_LOG_WARN;
    /* The interesting lines come from the driver and its supplicant, and they
     * are spread across several tags. Turning on only "wifi" leaves out the
     * wpa_supplicant and the state machine lines that carry the real codes. */
    esp_log_level_set("wifi", lvl);
    esp_log_level_set("wpa_supplicant", lvl);
    esp_log_level_set("phy_init", lvl);
    esp_log_level_set("phy", lvl);
    ESP_LOGW(TAG, "wifi driver log level -> %s", on ? "DEBUG" : "WARN");
}

static bool mac_custom_set;
static uint8_t mac_custom[6];

void wifi_set_pmf(bool on);

void wifi_set_mac(const char *txt)
{
    if (txt == NULL || txt[0] == '\0' || strcmp(txt, "off") == 0) {
        mac_custom_set = false;
        ESP_LOGW(TAG, "custom MAC cleared, hardware MAC will be used");
    } else if (mac_from_text(txt, mac_custom)) {
        mac_custom_set = true;
        ESP_LOGW(TAG, "custom MAC set to %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_custom[0], mac_custom[1], mac_custom[2],
                 mac_custom[3], mac_custom[4], mac_custom[5]);
    } else {
        ESP_LOGE(TAG, "bad MAC, use aa:bb:cc:dd:ee:ff or 'off'");
        return;
    }

    /* Apply it now rather than only at boot. The driver is initialised by this
     * point, and esp_wifi_set_mac() takes effect on the next start; the restart
     * below in wifi_set_pmf() is what actually makes the STA use it. */
    if (mac_custom_set) {
        esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac_custom);
        ESP_LOGW(TAG, "esp_wifi_set_mac -> %s", esp_err_to_name(e));
    }
    wifi_set_pmf((int)auth_mode);
}

/* Runtime password override. The 4-way handshake timing out is the classic
 * signature of a rejected PSK, and the screenshot's font makes "0" and "O"
 * indistinguishable. Trying candidates from the console settles it without a
 * rebuild per guess. */
static char pw_override[64];

const char *wifi_current_password(void)
{
    return pw_override[0] ? pw_override : WIFI_PASSWORD;
}

void wifi_set_auth_mode(int mode);

void wifi_set_bssid(const char *txt)
{
    if (txt == NULL || txt[0] == '\0' || strcmp(txt, "off") == 0) {
        bssid_locked = false;
        ESP_LOGW(TAG, "BSSID lock cleared");
    } else if (mac_from_text(txt, bssid_lock)) {
        bssid_locked = true;
        ESP_LOGW(TAG, "locked to BSSID %02x:%02x:%02x:%02x:%02x:%02x",
                 bssid_lock[0], bssid_lock[1], bssid_lock[2],
                 bssid_lock[3], bssid_lock[4], bssid_lock[5]);
    } else {
        ESP_LOGE(TAG, "bad MAC, use aa:bb:cc:dd:ee:ff or 'off'");
        return;
    }
    wifi_set_auth_mode((int)auth_mode);
}

void wifi_set_pmf(bool on)
{
    pmf_capable = on;
    wifi_set_auth_mode((int)auth_mode);
    ESP_LOGW(TAG, "PMF capable = %s, reconnecting", on ? "yes" : "no");
}

void wifi_set_password(const char *pw)
{
    strncpy(pw_override, pw ? pw : "", sizeof(pw_override) - 1);
    ESP_LOGW(TAG, "password override set, length=%d, reconnecting",
             (int)strlen(pw_override));
    wifi_set_auth_mode((int)auth_mode);
}

/* Access-point fallback.
 *
 * The STA link to home4 is not completing (reason=2 AUTH_EXPIRE during the
 * 4-way handshake, while the AP is plainly visible as WPA2-PSK at -59 dBm).
 * Rather than leave the web interface unusable until that is resolved, the
 * device brings up its own AP so the UI can be exercised on the bench.
 * The AP is deliberately open: this is a bench device on a workbench, and an
 * open AP is easier to join than one whose password also has to be explained.
 */
#define AP_SSID     "ESP32-Scale"
#define AP_CHANNEL  1
#define AP_FALLBACK_SECONDS 15

static bool ap_mode = false;
static bool fallback_done = false;

/* ------------------------------------------------------------------ WiFi */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(wifi_events, WIFI_DRIVER_READY_BIT);
        if (!wifi_scanning && wifi_config_want_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The state-machine log prints internal status words (0xb0, 0x200) that
         * are not documented reason codes. The disconnect event carries the real
         * one, which is what identifies auth failures: 102 handshake timeout,
         * 112 = 4-way handshake, 202 auth fail. */
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected, reason=%d", (int)e->reason);
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (wifi_scanning) {
            return;
        }
        /* Back off instead of reconnecting immediately on every event. Hammering
         * the AP while it is still tearing the previous association down keeps
         * us stuck in a retry loop. */
        wifi_retry_ms = (wifi_retry_ms == 0) ? 250 : wifi_retry_ms * 2;
        /* Capped at 20 s. The earlier 8 s cap meant a rejected station sent
         * association requests 7-8 times a minute, which is the pattern an AP
         * treats as an attack and can answer by dropping the client outright. */
        if (wifi_retry_ms > 20000) {
            wifi_retry_ms = 20000;
        }
        vTaskDelay(pdMS_TO_TICKS(wifi_retry_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "up: http://" IPSTR ".%d", IP2STR(&e->ip_info.ip),
                 e->ip_info.gw.addr & 0xff);
        wifi_retry_ms = 0;
        sta_ever_connected = true;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_apply_config(void)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, wifi_current_password(),
            sizeof(wc.sta.password) - 1);

    /* Explicit scan/roaming policy. These have driver defaults that can leave
     * a narrow band or an aggressive roam policy in place; being explicit costs
     * nothing and removes a variable when a single AP refuses association. */
    wc.sta.scan_method = WIFI_FAST_SCAN;   /* keeps the scan cache warm for reconnects */

    if (bssid_locked) {
        memcpy(wc.sta.bssid, bssid_lock, 6);
        wc.sta.bssid_set = true;
    }
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.rssi = -80;   /* anything above the noise floor qualifies */

    switch (auth_mode) {
    case AUTH_MODE_WPA3_SAE:
        wc.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
        wc.sta.pmf_cfg.required = true;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.pmf_cfg.required = true;
        break;
    case AUTH_MODE_WPA2_WPA3:
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.pmf_cfg.required = false;
        break;
    case AUTH_MODE_WPA2:
    default:
        /* Threshold left at 0 (accept any). Pinning it to WPA2_PSK made the AP
         * stop matching entirely (reason=211), which hides the real handshake
         * failure behind a filtering error. */
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = pmf_capable;
        wc.sta.pmf_cfg.required = false;
        break;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config FAILED: %s - the password did NOT reach "
                      "the chip", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "auth mode %d applied", (int)auth_mode);
}

void wifi_set_auth_mode(int m)
{
    if (m < 0 || m > 2) {
        return;
    }
    auth_mode = (auth_mode_sel_t)m;

    /* Same ordering trap: stop, apply config, then start and connect. */
    esp_wifi_stop();
    wifi_config_want_connect = false;
    /* set_mode is required after stop(): without it set_config reports
     * ESP_ERR_WIFI_MODE and the credentials silently never apply. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_apply_config();
    esp_wifi_start();
    xEventGroupWaitBits(wifi_events, WIFI_DRIVER_READY_BIT, pdTRUE, pdFALSE,
                        pdMS_TO_TICKS(2000));
    wifi_config_want_connect = true;
    esp_wifi_connect();
}

static void sta_retry_task(void *arg);

static void wifi_start(void)
{
    wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    /* The AP netif is created up front. Doing it later, while the STA netif is
     * live, races and faults. */
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Must be set here, before esp_wifi_start(): after start the driver ignores
     * esp_wifi_set_mac() for the STA interface. Useful for testing whether the
     * AP is refusing this particular MAC. */
    if (mac_custom_set) {
        ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, mac_custom));
        ESP_LOGW(TAG, "STA MAC overridden");
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* Country must match the router (RU, seen in its web UI). IDF defaults to
     * "01" world-safe mode capped at channel 11, and home4 sits exactly on
     * channel 11, so the default domain is one channel narrower than we need.
     * Policy is MANUAL so the AP's own 802.11d advertisement cannot pull the
     * driver back to the narrow default. */
    wifi_country_t country = {
        .cc = "RU",
        .schan = 1,
        .nchan = 13,
        /* Reduced from 20 dBm. The board resets with POWERON while WiFi is up,
         * which is a power-supply symptom, not a firmware one. TX power is the
         * single biggest current draw in this design, so backing it off is the
         * cheapest thing to try before blaming the cable or the port. */
        .max_tx_power = 8.5,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_err_t cerr = esp_wifi_set_country(&country);
    ESP_LOGI(TAG, "country RU ch1-13 manual -> %s", esp_err_to_name(cerr));


    /* Order matters and the failure is silent if you get it wrong.
     *
     * esp_wifi_set_mode() starts the connection attempt by itself. Calling
     * esp_wifi_set_config() afterwards is rejected with
     * "wifi:sta is connecting, cannot set config" / ESP_ERR_WIFI_STATE, so the
     * password NEVER reaches the chip and every attempt runs unauthenticated -
     * which surfaces as reason=211 and looks exactly like a bad password or a
     * missing AP. The config must be applied BEFORE esp_wifi_start(), and the
     * first connect must be suppressed so the driver is idle when we do it. */
    wifi_config_want_connect = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_apply_config();
    ESP_ERROR_CHECK(esp_wifi_start());

    /* TX power must be applied AFTER esp_wifi_start(), and then verified.
     *
     * Setting wifi_country_t.max_tx_power before the driver starts does not
     * stick: measured, a requested 8.5 dBm read back as 80 - the full-power
     * default. An over-driven transmitter is the documented cause of ESP32
     * stations stuck in "auth expired / association expired": the AP drops them
     * mid-authentication, which is exactly the reason=2 loop seen here, over and
     * over, with a password that a phone accepts without complaint.
     *
     * Measured on this IDF version: the setter takes dBm and the getter returns
     * dBm too - asking for 8 gave back 8. Earlier, with the limit only in
     * wifi_country_t before start, the getter returned 80, which is not a
     * physically meaningful transmit power and meant nothing was applied. */
    esp_err_t perr = esp_wifi_set_max_tx_power(8);
    int8_t tx_dbm = 0;
    esp_err_t gerr = esp_wifi_get_max_tx_power(&tx_dbm);
    ESP_LOGW(TAG, "TX power set 8 dBm -> %s, reads back %d dBm",
             esp_err_to_name(perr), (int)tx_dbm);
    if (gerr != ESP_OK || tx_dbm != 8) {
        ESP_LOGE(TAG, "TX power limit NOT in force (wanted 8, got %d)",
                 (int)tx_dbm);
    }

    /* Power save must be OFF. IDF enables WIFI_PS_MIN_MODEM by default, so the
     * station sleeps between DTIM beacons and stops answering the AP's polls.
     * The router then logs reason=4 DISASSOC_DUE_TO_INACTIVITY and drops us,
     * typically within seconds of associating. This device is mains-powered, so
     * there is nothing to gain by sleeping. */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "power save off -> %s", esp_err_to_name(ps));

    xEventGroupWaitBits(wifi_events, WIFI_DRIVER_READY_BIT,
                        pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    wifi_config_want_connect = true;
    esp_wifi_connect();

    ESP_LOGI(TAG, "connecting to %s", WIFI_SSID);
}

/* --------------------------------------------------------------- HTTP API */

static esp_err_t api_weight(httpd_req_t *req)
{
    scale_snapshot_t s;
    weight_task_get(&s);

    const char *unit = "g";
    int decimals = 2;
    switch (s.unit) {
    case UNIT_KILOGRAM:
        unit = "kg";
        decimals = 4;
        break;
    case UNIT_OUNCE:
        unit = "oz";
        decimals = 3;
        break;
    default:
        break;
    }

    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "{\"value\":%.4f,\"unit\":\"%s\",\"decimals\":%d,"
                     "\"grams\":%.2f,\"stable\":%s,\"zero\":%s,\"tare\":%s,"
                     "\"counts\":%ld,\"sigma_mg\":%.1f,\"cal\":%.2f,"
                     "\"tare_offset\":%ld,\"uptime\":%lu,\"azt\":%s,"
                     "\"heap_free\":%lu,\"heap_min\":%lu}",
                     (double)s.display_value, unit, decimals, (double)s.grams,
                     s.stable ? "true" : "false", s.zero ? "true" : "false",
                     s.tare_active ? "true" : "false", (long)s.counts,
                     (double)s.sigma_mg, (double)s.cal_factor,
                     (long)s.tare_offset, (unsigned long)s.uptime_s,
                     s.azt_enabled ? "true" : "false",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t api_tare(httpd_req_t *req)
{
    weight_task_tare();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);
    return ESP_OK;
}

static esp_err_t api_unit(httpd_req_t *req)
{
    /* ESP-IDF v6 split this into two calls: length first, then copy. */
    char q[32] = {0};
    if (httpd_req_get_url_query_len(req) < sizeof(q)) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        if (q[0] == 'u' && q[1] == '=') {
            int u = atoi(&q[2]);
            if (u >= 0 && u <= 2) {
                weight_task_set_unit((scale_unit_t)u);
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);
    return ESP_OK;
}

static esp_err_t api_azt(httpd_req_t *req)
{
    bool on = true;
    char q[32] = {0};
    if (httpd_req_get_url_query_len(req) < sizeof(q)) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        if (strstr(q, "off")) {
            on = false;
        }
    }
    weight_task_set_azt(on);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, on ? "{\"azt\":true}" : "{\"azt\":false}",
                    on ? 12 : 13);
    return ESP_OK;
}

/* Calibration master. The measurement happens in its own task; these handlers
 * only start it and read its published state, so the HTTP server never blocks
 * for the ~30 s the sequence takes. */
static esp_err_t api_cal_start(httpd_req_t *req)
{
    double mass = 0.0;
    char q[48] = {0};
    if (httpd_req_get_url_query_len(req) < sizeof(q)) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        char *m = strstr(q, "mass=");
        if (m) {
            mass = atof(m + 5);
        }
    }

    esp_err_t err = weight_task_cal_start(mass);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_send(req, "{\"error\":\"already running\"}", 25);
        return err;
    }
    const char *reply = mass > 0.0 ? "{\"started\":true,\"apply\":true}"
                                   : "{\"started\":true,\"apply\":false}";
    httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_cal_state(httpd_req_t *req)
{
    cal_state_t c;
    weight_task_cal_get(&c);

    static const char *names[] = {
        "idle", "settling", "wait_load", "measuring", "done", "abort",
    };
    const char *phase = (c.phase >= CAL_IDLE && c.phase <= CAL_ABORT)
                            ? names[c.phase] : "idle";

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"phase\":\"%s\",\"tare\":%ld,\"delta\":%.0f,"
                     "\"grams\":%.4f,\"new_cal\":%.2f,\"saved\":%s,"
                     "\"elapsed_ms\":%lld,\"detail\":\"%s\"}",
                     phase, (long)c.tare, c.delta, c.grams, c.new_cal,
                     c.saved ? "true" : "false", (long long)c.elapsed_ms,
                     c.detail);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* --------------------------------------------------------------- the page */

/*
 * Full-bleed tiles where the colour change itself is the divider, per DESIGN.md
 * "Do's". Tiles are never rounded. One blue accent only. Weight readout carries
 * the single product shadow.
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* The page is firmware, not a document. A copy sitting in the browser cache
     * is indistinguishable from a build that did not take, and it keeps serving
     * buttons and handlers that no longer exist. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    /* One plain send with an explicit Content-Length.
     *
     * A chunked version was tried and was wrong: curl reported
     * "Leftovers after chunking: 1351 bytes" and discarded the whole body, which
     * is indistinguishable in a browser from a dead server. With Content-Length
     * there is nothing for the two ends to disagree about.
     *
     * This only became possible after the real fix. Previously this same call
     * stalled after one 1440-byte TCP segment, which is the signature of a send
     * buffer that never drains - lwIP's TCP/IP thread was starved on Core 0 by
     * the measurement task and stopped processing incoming acknowledgements. */
    const char *body = INDEX_HTML;
    size_t body_len = INDEX_HTML_LEN;

    /* Serve the gzipped build when the client asks for it.
     *
     * Measured 2026-10-09, the real reason the page "does not open": the link to
     * the access point loses half its packets at 347 ms average round trip, which
     * is about 400 bytes per second of goodput. The 13 666 byte page needs about
     * 35 s at that rate and in practice never arrived - while the 221 byte API
     * response came back in 0.09 s. The device's own log said it had handed all
     * 13 666 bytes to the network stack in 3 ms, so nothing on this device was
     * slow; there were simply far too many bytes to push over a bad link.
     *
     * gzip takes the same page to 4 490 bytes, one third of the traffic, for zero
     * runtime cost: it is compressed once by tools/embed_web.py at build time.
     * Every browser sends Accept-Encoding: gzip. This IDF version has no
     * req->accept_encoding field, so the header is read by hand.
     *
     * This shortens the transfer, it does not repair the link. At 400 bytes per
     * second even 4 490 bytes takes about 11 s, so the radio path still needs
     * attention - see AGENTS.md. */
    char enc[48] = {0};
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) == ESP_OK &&
        strstr(enc, "gzip")) {
        body = INDEX_HTML_GZ;
        body_len = INDEX_HTML_GZ_LEN;
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = httpd_resp_send(req, body, body_len);
    ESP_LOGW(TAG, "page: %d bytes%s, err=%s, took %lld ms",
             (int)body_len, body == INDEX_HTML_GZ ? " gzip" : " raw",
             esp_err_to_name(err),
             (long long)((esp_timer_get_time() - t0) / 1000));
    return err;
}

int wifi_get_auth_mode(void)
{
    return (int)auth_mode;
}

/*
 * Brings up the fallback AP WITHOUT touching the station.
 *
 * This used to stop the driver and restart it in AP-only mode, then retry home4
 * five minutes later. That was a bad trade: stopping the radio tore down a
 * perfectly good association (reason=8, ASSOC_LEAVE) and left the device
 * unreachable from home4 for minutes at a time, which reads as "the web
 * interface is dead".
 *
 * The driver supports AP and STA at the same time, so nothing needs to stop.
 * Measured case that prompted this: the station associated fine at rssi -74 and
 * then simply never got a DHCP lease. Killing a working association because an
 * address was slow to arrive is exactly backwards.
 *
 * From here the station keeps retrying in the background while the AP stays up,
 * so the device is reachable at 192.168.4.1 no matter what the router does.
 */
static void ap_start(void)
{
    if (ap_mode) {
        return;
    }
    ap_mode = true;
    fallback_done = true;

    /* APSTA, on the already-running driver. No stop, no start, no lost
     * association. */
    esp_err_t e2 = esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;

    /* set_config can reject with ESP_ERR_WIFI_MODE if it lands before the driver
     * has settled into AP mode, so give it a moment and retry. */
    esp_err_t e3 = ESP_ERR_WIFI_MODE;
    for (int i = 0; i < 5 && e3 != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        e3 = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }

    ESP_LOGW(TAG, "fallback AP added alongside STA: mode=%s cfg=%s",
             esp_err_to_name(e2), esp_err_to_name(e3));
    ESP_LOGW(TAG, "fallback AP '%s' is live at http://192.168.4.1, "
                  "station keeps retrying home4", AP_SSID);
    printf("#AP_MODE,ssid=%s,url=http://192.168.4.1,note=APSTA,station_still_trying\n",
           AP_SSID);
    fflush(stdout);

    /* The AP is a fallback, not a destination. Without this the device stays on
     * its own network forever after a single failure and silently disappears
     * from home4, which reads as "the web interface is broken". */
    xTaskCreate(sta_retry_task, "sta_retry", 3072, NULL, 3, NULL);
}

/* Nudges the station to try home4 again. No driver restart, no mode change -
 * under APSTA a plain reconnect request is all that is needed. */
static volatile bool sta_retry_paused = false;
static bool wifi_suspended;

void wifi_leave_ap_mode(void)
{
    if (!ap_mode) {
        return;
    }
    if (wifi_suspended || sta_retry_paused) {
        return;
    }
    if (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) {
        return;
    }
    wifi_config_want_connect = true;
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "retrying home4 alongside the fallback AP");
    }
}


/* Leaving the soft AP means esp_wifi_stop() followed by esp_wifi_start(): a full
 * driver restart. Doing that on a 30 s cycle, for as long as the AP refuses us,
 * hammers the PHY and the NVS calibration data - measured, the radio went deaf
 * within minutes and the scan returned ap_num=0 until the flash was erased.
 *
 * So the retry task now only ever runs when the device is actually parked in AP
 * mode, and does so every 5 minutes instead of every 30 seconds. Once the station
 * is in STA mode the disconnect handler reconnects on its own with backoff, and
 * no driver restart is involved at all. */
static void sta_retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (sta_retry_paused || wifi_suspended) {
            continue;   /* scan/diagnostics or deliberate shutdown */
        }
        if (!ap_mode) {
            continue;   /* STA is already handling its own reconnects */
        }
        if (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) {
            continue;
        }
        ESP_LOGI(TAG, "still not on home4, retrying");
        wifi_leave_ap_mode();
    }
}

/* Suspends or resumes the WiFi radio without tearing down netif or the event
 * loop. Diagnostic only: the raw noise floor must be compared with the radio
 * silent, because WiFi TX is the largest current draw in this design and the
 * HX711 is powered from the same 3.3 V rail. Noise that drops with the radio off
 * is a supply symptom, not a mechanical one. */
int wifi_set_tx_power(int dbm)
{
    if (dbm < 0 || dbm > 20) {
        ESP_LOGE(TAG, "TX power %d dBm out of range 0..20", dbm);
        return INT8_MIN;
    }
    esp_err_t err = esp_wifi_set_max_tx_power((int8_t)dbm);
    int8_t back = 0;
    esp_err_t gerr = esp_wifi_get_max_tx_power(&back);
    ESP_LOGW(TAG, "TX power %d dBm -> %s, reads back %d dBm",
             dbm, esp_err_to_name(err), (int)back);
    return gerr == ESP_OK ? (int)back : INT8_MIN;
}

void wifi_suspend(bool off)
{
    if (off) {
        wifi_suspended = true;
        esp_wifi_stop();
        ESP_LOGW(TAG, "WiFi radio suspended");
        return;
    }

    wifi_suspended = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_apply_config();
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resume failed: %s", esp_err_to_name(err));
        return;
    }
    xEventGroupWaitBits(wifi_events, WIFI_DRIVER_READY_BIT, pdTRUE, pdFALSE,
                        pdMS_TO_TICKS(2000));
    wifi_config_want_connect = true;
    esp_wifi_connect();
    ESP_LOGW(TAG, "WiFi radio resumed");
}

/* Called once the STA attempt has had its chance. */
void wifi_start_ap_fallback(void)
{
    if (wifi_suspended) {
        return;
    }
    if (xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(AP_FALLBACK_SECONDS * 1000)) !=
        WIFI_CONNECTED_BIT) {
        ap_start();
    }
}

/* Self-test: fetch our own page over loopback and report what came back.
 * A missing quote in the embedded HTML compiles fine and serves a broken page,
 * so this is checked rather than assumed. */
void web_selftest(void)
{
    esp_http_client_config_t cc = {
        .url = "http://127.0.0.1/",
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (c == NULL) {
        printf("#SELFTEST,client init failed\n");
        return;
    }
    /* open() then perform() issues the request; read() only drains the body,
     * so without perform() the server is never asked and status stays 0. */
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        err = esp_http_client_perform(c);
    }
    if (err != ESP_OK) {
        printf("#SELFTEST,perform failed: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return;
    }
    /* perform() has already consumed the body, so read() returns 0. Take the
     * declared content length instead - that is the number we actually want. */
    int total = (int)esp_http_client_get_content_length(c);
    int status = esp_http_client_get_status_code(c);
    printf("#SELFTEST,status=%d,declared_bytes=%d\n", status, total);
    if (total < 1000) {
        printf("#SELFTEST,WARNING page looks truncated\n");
    } else {
        printf("#SELFTEST,OK page served\n");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    fflush(stdout);
}

void wifi_scan_report(void)
{
    /* IDF v6 takes a caller-provided array and scan_time instead of active_scan.
     * phy_11a matters: ESP32 is 2.4 GHz only, so a 5 GHz AP will never connect
     * no matter how correct the password is. */
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_time = {
            .active = {.min = 120, .max = 300},
            .passive = 300,
        },
    };
    uint16_t found = 0;

    /* The station retries auth in a tight loop when the password is wrong, and a
     * scan is refused in that state. Detach first, scan, then reconnect. */
    wifi_scanning = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(600));

    /* A scan can be refused while the station is mid-connection, so retry
     * briefly and report the actual error rather than a bare "failed". */
    /* Pause the reconnect task so it cannot yank the radio mid-scan, and leave
     * the soft AP first: a station and an AP cannot scan for each other. */
    sta_retry_paused = true;

    if (ap_mode) {
        esp_wifi_stop();
        ap_mode = false;
        wifi_config_want_connect = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_apply_config();
        esp_wifi_start();
        xEventGroupWaitBits(wifi_events, WIFI_DRIVER_READY_BIT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(2000));
        wifi_config_want_connect = true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        err = esp_wifi_scan_start(&sc, true);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (err != ESP_OK) {
        printf("#SCAN,start failed: %s\n", esp_err_to_name(err));
        wifi_scanning = false;
        sta_retry_paused = false;
        esp_wifi_connect();
        return;
    }
    /* Query the count first and size the array to it. A fixed 20-entry array can
     * silently truncate, and a static one costs IRAM the scan buffers also want. */
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK || found == 0) {
        printf("#SCAN,ap_num=0\n");
        wifi_scanning = false;
        sta_retry_paused = false;
        esp_wifi_connect();
        return;
    }
    printf("#SCAN,ap_num=%d\n", (int)found);

    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        printf("#SCAN,out of memory\n");
        wifi_scanning = false;
        sta_retry_paused = false;
        esp_wifi_connect();
        return;
    }
    uint16_t got = found;
    esp_err_t rec_err = esp_wifi_scan_get_ap_records(&got, recs);
    printf("#SCAN,records_err=%s,got=%d\n", esp_err_to_name(rec_err), (int)got);
    if (rec_err != ESP_OK) {
        free(recs);
        wifi_scanning = false;
        sta_retry_paused = false;
        esp_wifi_connect();
        return;
    }
    found = got;

    for (uint16_t i = 0; i < found; i++) {
        const char *sec;
        switch (recs[i].authmode) {
        case WIFI_AUTH_OPEN: sec = "OPEN"; break;
        case WIFI_AUTH_WEP: sec = "WEP"; break;
        case WIFI_AUTH_WPA_PSK: sec = "WPA-PSK"; break;
        case WIFI_AUTH_WPA2_PSK: sec = "WPA2-PSK"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: sec = "WPA/WPA2"; break;
        case WIFI_AUTH_WPA3_PSK: sec = "WPA3-PSK"; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: sec = "WPA2/WPA3"; break;
        case WIFI_AUTH_OWE: sec = "OWE"; break;
        default: sec = "other"; break;
        }
        printf("#AP,ssid=%s,rssi=%d,ch=%d,sec=%s,band=%s,bssid=%02x:%02x:%02x:"
               "%02x:%02x:%02x\n",
               (char *)recs[i].ssid, (int)recs[i].rssi, (int)recs[i].primary,
               sec, recs[i].phy_11a ? "5GHz(unsupported)" : "2.4GHz",
               recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
               recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
    }
    printf("#SCAN_END\n");
    fflush(stdout);

    free(recs);
    wifi_scanning = false;
    sta_retry_paused = false;
    esp_wifi_connect();
}

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* The server task inherits the caller's priority in IDF v6 (there is no
     * server_prio field). net_start() runs from app_main at priority 1, so the
     * HTTP server would be served below the measurement task - which is exactly
     * the wrong way round while the scale is streaming at 11 Hz. Raise the
     * calling task first so the server inherits the higher priority. */
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 6);

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    ESP_LOGI(TAG, "http server ready at task priority %d",
             (int)uxTaskPriorityGet(NULL));

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler,
    };
    httpd_uri_t w = {
        .uri = "/api/weight", .method = HTTP_GET, .handler = api_weight,
    };
    httpd_uri_t t = {
        .uri = "/api/tare", .method = HTTP_POST, .handler = api_tare,
    };
    httpd_uri_t u = {
        .uri = "/api/unit", .method = HTTP_POST, .handler = api_unit,
    };
    httpd_uri_t a = {
        .uri = "/api/azt", .method = HTTP_POST, .handler = api_azt,
    };
    httpd_uri_t cstart = {
        .uri = "/api/cal/start", .method = HTTP_POST, .handler = api_cal_start,
    };
    httpd_uri_t cstate = {
        .uri = "/api/cal", .method = HTTP_GET, .handler = api_cal_state,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &w));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &t));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &a));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cstart));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cstate));

    ESP_LOGI(TAG, "http server ready");
}

void net_start(void)
{
    wifi_start();
    web_server_start();
}