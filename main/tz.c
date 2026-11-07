#include "tz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"

static const char *TAG = "tz";

#define TZ_CACHE_SIZE 32

typedef struct {
    char key[8];
    int  offset_s;
    bool valid;
    bool used;
    int64_t fetched_us;   /* when the offset was looked up */
} tz_entry_t;

/* A UTC offset is only true for the moment it was looked up: daylight
 * saving moves it twice a year. Cached airport offsets expire after this. */
#define TZ_MAX_AGE_US (6LL * 3600 * 1000000)

static tz_entry_t s_cache[TZ_CACHE_SIZE];
static int s_next;

static int s_home_offset;
static bool s_home_known;

void tz_set_home_offset(int offset_s)
{
    s_home_offset = offset_s;
    s_home_known = true;
    ESP_LOGI(TAG, "home timezone: UTC%+d min", offset_s / 60);
}

bool tz_home_known(void)
{
    return s_home_known;
}

int tz_home_offset(void)
{
    return s_home_offset;
}

/* Current UTC offset at a location from Open-Meteo (timezone=auto),
 * which already includes daylight saving for today. */
static bool fetch_offset(double lat, double lon, int *offset_s)
{
    bool ok = false;
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&timezone=auto&forecast_days=1",
             lat, lon);
    char *buf = malloc(2048);
    if (buf == NULL) {
        return false;
    }
    if (http_get_to_buffer_t(url, buf, 2048, NULL, 5000) == ESP_OK) {
        cJSON *root = cJSON_Parse(buf);
        if (root != NULL) {
            const cJSON *jo = cJSON_GetObjectItem(root, "utc_offset_seconds");
            if (cJSON_IsNumber(jo)) {
                *offset_s = (int)jo->valuedouble;
                ok = true;
            }
            cJSON_Delete(root);
        }
    }
    free(buf);
    return ok;
}

bool tz_refresh_home(double lat, double lon)
{
    int off;
    if (!fetch_offset(lat, lon, &off)) {
        return false;
    }
    if (!s_home_known || off != s_home_offset) {
        tz_set_home_offset(off);
    }
    return true;
}

bool tz_offset_for(const char *key, double lat, double lon, int *offset_s)
{
    if (key == NULL || key[0] == '\0') {
        return false;
    }
    for (int i = 0; i < TZ_CACHE_SIZE; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].key, key) == 0) {
            if (s_cache[i].valid &&
                esp_timer_get_time() - s_cache[i].fetched_us > TZ_MAX_AGE_US) {
                /* stale (clocks may have changed since): look it up again */
                s_cache[i].used = false;
                s_cache[i].key[0] = '\0';
                break;
            }
            *offset_s = s_cache[i].offset_s;
            return s_cache[i].valid;
        }
    }

    tz_entry_t *slot = &s_cache[s_next];
    s_next = (s_next + 1) % TZ_CACHE_SIZE;
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->key, key, sizeof(slot->key));
    slot->used = true;

    slot->valid = fetch_offset(lat, lon, &slot->offset_s);
    slot->fetched_us = esp_timer_get_time();
    if (!slot->valid) {
        /* transport failure: forget the slot so the next lookup retries
         * instead of answering "unknown tz" for the rest of the session */
        slot->used = false;
        slot->key[0] = '\0';
    }
    ESP_LOGI(TAG, "%s -> %+d s%s", key, slot->offset_s, slot->valid ? "" : " (failed)");
    *offset_s = slot->offset_s;
    return slot->valid;
}
