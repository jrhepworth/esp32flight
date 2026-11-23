#include "wifi_mgr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
#define BIT_CONNECTED BIT0

static volatile bool s_scanning;     /* pause the retry loop during scans */
static volatile int s_last_reason;   /* last STA disconnect reason */
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    if (!s_scanning && settings_get()->wifi_ssid[0] != '\0') {
        esp_wifi_connect();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    bool have_ssid = settings_get()->wifi_ssid[0] != '\0';
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (have_ssid) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        s_last_reason = ev != NULL ? ev->reason : 0;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (have_ssid && !s_scanning) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry in 3 s", s_last_reason);
            /* never block the event task; a one-shot timer reconnects */
            if (s_retry_timer != NULL) {
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, 3000000);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_mgr_start(void)
{
    const settings_t *st = settings_get();

    s_events = xEventGroupCreate();
    const esp_timer_create_args_t targs = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    esp_timer_create(&targs, &s_retry_timer);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t werr = esp_wifi_init(&cfg);
    if (werr != ESP_OK) {
        /* Tab5: the C6 radio may be absent/unresponsive; a desk radar
           with a dead radio should still show its UI, not bootloop */
        ESP_LOGE(TAG, "wifi init failed (%s); running without radio",
                 esp_err_to_name(werr));
        return werr;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (st->wifi_ssid[0] != '\0') {
        wifi_config_t wifi_config = { 0 };
        strlcpy((char *)wifi_config.sta.ssid, st->wifi_ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, st->wifi_pass, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = st->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        /* the default fast scan joins the first matching AP it hears,
         * often a distant mesh node or extender; scan every channel and
         * take the strongest (also on each reconnect) */
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_LOGI(TAG, "connecting to \"%s\"", st->wifi_ssid);
    } else {
        ESP_LOGW(TAG, "no SSID configured; Wi-Fi up for scanning only");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* mains-powered display: the default modem sleep skips beacons, which
     * on a weak link adds latency and lets the AP drop us (beacon
     * timeout). Stay awake; not fatal where the radio ignores it (Tab5). */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "power save off failed: %s", esp_err_to_name(ps_err));
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t *records, uint16_t *count)
{
    /* a station stuck in a connect-retry loop reports zero networks;
       pause the loop, drop the half-open attempt, then scan */
    s_scanning = true;
    if (s_retry_timer != NULL) {
        esp_timer_stop(s_retry_timer);
    }
    if (!wifi_mgr_is_connected()) {
        esp_wifi_disconnect();
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    } else {
        err = esp_wifi_scan_get_ap_records(count, records);
    }
    s_scanning = false;
    if (!wifi_mgr_is_connected() && settings_get()->wifi_ssid[0] != '\0') {
        esp_wifi_connect();
    }
    return err;
}

int wifi_mgr_last_reason(void)
{
    return s_last_reason;
}

bool wifi_mgr_wait_connected(int timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

bool wifi_mgr_is_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}
