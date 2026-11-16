#include "http_util.h"

#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "http";

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} sink_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    sink_t *s = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && s != NULL) {
        size_t space = s->cap - 1 - s->len;
        size_t n = evt->data_len;
        if (n > space) {
            n = space;
            s->overflow = true;
        }
        memcpy(s->buf + s->len, evt->data, n);
        s->len += n;
    }
    return ESP_OK;
}

esp_err_t http_get_to_buffer(const char *url, char *buf, size_t buf_size, size_t *out_len)
{
    return http_get_to_buffer_t(url, buf, buf_size, out_len, 12000);
}

static esp_err_t get_with_hdr_t(const char *url, char *buf, size_t buf_size, size_t *out_len,
                                const char *hdr_key, const char *hdr_val, int timeout_ms);

esp_err_t http_get_to_buffer_t(const char *url, char *buf, size_t buf_size, size_t *out_len,
                               int timeout_ms)
{
    return get_with_hdr_t(url, buf, buf_size, out_len, NULL, NULL, timeout_ms);
}

esp_err_t http_get_to_buffer_hdr(const char *url, char *buf, size_t buf_size, size_t *out_len,
                                 const char *hdr_key, const char *hdr_val)
{
    return get_with_hdr_t(url, buf, buf_size, out_len, hdr_key, hdr_val, 12000);
}

static esp_err_t get_with_hdr_t(const char *url, char *buf, size_t buf_size, size_t *out_len,
                                const char *hdr_key, const char *hdr_val, int timeout_ms)
{
    sink_t sink = { .buf = buf, .cap = buf_size, .len = 0, .overflow = false };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_cb,
        .user_data = &sink,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .user_agent = "esp32flight/0.4 (+https://github.com/theqkash/esp32flight)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    if (hdr_key != NULL && hdr_val != NULL) {
        esp_http_client_set_header(client, hdr_key, hdr_val);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    buf[sink.len] = '\0';
    if (out_len != NULL) {
        *out_len = sink.len;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        return ESP_ERR_HTTP_BASE + status;
    }
    if (sink.overflow) {
        ESP_LOGW(TAG, "GET %s: response truncated at %u bytes", url, (unsigned)sink.len);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t http_post_to_buffer(const char *url, const char *body,
                              char *buf, size_t buf_size)
{
    return http_post_to_buffer_t(url, body, buf, buf_size, 12000);
}

esp_err_t http_post_to_buffer_t(const char *url, const char *body,
                                char *buf, size_t buf_size, int timeout_ms)
{
    sink_t sink = { .buf = buf, .cap = buf_size, .len = 0, .overflow = false };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_cb,
        .user_data = &sink,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "esp32flight/0.4 (+https://github.com/theqkash/esp32flight)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    buf[sink.len] = '\0';
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST %s -> HTTP %d", url, status);
        return ESP_ERR_HTTP_BASE + status;
    }
    return ESP_OK;
}

esp_err_t http_post_text(const char *url, const char *body,
                         const char *hdr_key, const char *hdr_val)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "esp32flight/0.4 (+https://github.com/theqkash/esp32flight)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    if (hdr_key != NULL && hdr_val != NULL) {
        esp_http_client_set_header(client, hdr_key, hdr_val);
    }
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        err = ESP_FAIL;
    }
    return err;
}

static esp_http_client_handle_t s_ka_client[HTTP_KEEPALIVE_SLOTS];
static sink_t s_ka_sink[HTTP_KEEPALIVE_SLOTS];

esp_err_t http_get_keepalive(int slot, const char *url,
                             char *buf, size_t buf_size, size_t *out_len)
{
    return http_get_keepalive_t(slot, url, buf, buf_size, out_len, 12000);
}

esp_err_t http_get_keepalive_t(int slot, const char *url,
                               char *buf, size_t buf_size, size_t *out_len,
                               int timeout_ms)
{
    if (slot < 0 || slot >= HTTP_KEEPALIVE_SLOTS) {
        return http_get_to_buffer_t(url, buf, buf_size, out_len, timeout_ms);
    }
    sink_t *sink = &s_ka_sink[slot];
    bool reused = s_ka_client[slot] != NULL;
retry_fresh:
    sink->buf = buf;
    sink->cap = buf_size;
    sink->len = 0;
    sink->overflow = false;

    if (s_ka_client[slot] == NULL) {
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_cb,
            .user_data = sink,
            .timeout_ms = timeout_ms,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = false,
            .keep_alive_enable = true,
            .user_agent = "esp32flight/0.4 (+https://github.com/theqkash/esp32flight)",
        };
        s_ka_client[slot] = esp_http_client_init(&config);
        if (s_ka_client[slot] == NULL) {
            return ESP_FAIL;
        }
    } else {
        esp_http_client_set_url(s_ka_client[slot], url);
        /* the timeout is fixed at init; one slot serves callers with
         * different budgets (primary fetch vs. the quick union pass) */
        esp_http_client_set_timeout_ms(s_ka_client[slot], timeout_ms);
    }

    esp_err_t err = esp_http_client_perform(s_ka_client[slot]);
    int status = err == ESP_OK ? esp_http_client_get_status_code(s_ka_client[slot]) : 0;

    buf[sink->len] = '\0';
    if (out_len != NULL) {
        *out_len = sink->len;
    }
    if (err == ESP_OK && status >= 200 && status < 300 && !sink->overflow) {
        return ESP_OK;   /* connection stays open for the next cycle */
    }
    if (err == ESP_OK && status >= 300 && !sink->overflow) {
        /* an HTTP-level answer (adsbdb says 404 for "no route" dozens of
         * times a session) is a healthy connection: keep it - tearing it
         * down made every enrichment miss cost a fresh TLS handshake */
        ESP_LOGW(TAG, "GET(ka) %s -> HTTP %d", url, status);
        return ESP_ERR_HTTP_BASE + status;
    }
    /* transport trouble: drop the cached connection, start clean next time */
    esp_http_client_cleanup(s_ka_client[slot]);
    s_ka_client[slot] = NULL;
    if (err != ESP_OK) {
        if (reused) {
            /* Servers close idle connections between our cycles; the first
             * request on a stale socket then fails. Without this retry the
             * caller falls over to another aggregator with different
             * coverage and aircraft blink out for a cycle. One immediate
             * fresh-connection attempt makes staleness invisible. */
            reused = false;
            goto retry_fresh;
        }
        ESP_LOGW(TAG, "GET(ka) %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (sink->overflow) {
        ESP_LOGW(TAG, "GET(ka) %s: response truncated at %u bytes", url,
                 (unsigned)sink->len);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "GET(ka) %s -> HTTP %d", url, status);
    return ESP_ERR_HTTP_BASE + status;
}
