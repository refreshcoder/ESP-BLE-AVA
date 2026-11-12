#include "radio.h"
#include "app_cfg.h"
#include "app_log.h"
#include "beacon_adv.h"
#include "beacon_scan.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "radio";

static SemaphoreHandle_t s_mtx;
static volatile rmode_t  s_mode = RMODE_IDLE;

const char *radio_mode_str(rmode_t m)
{
    switch (m) {
    case RMODE_SNIFF: return "sniff";
    case RMODE_CLONE: return "clone";
    default:          return "idle";
    }
}

rmode_t radio_mode(void) { return s_mode; }

esp_err_t radio_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t radio_stop(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    blescan_stop();
    beacon_adv_stop();
    s_mode = RMODE_IDLE;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "mode=idle");
    return ESP_OK;
}

esp_err_t radio_sniff(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* Exclusive: fully stop advertising before starting the scan */
    beacon_adv_stop();

    cfg_lock();
    scan_cfg_t sc = cfg_get()->scan;
    cfg_unlock();

    esp_err_t e = blescan_start(&sc);
    s_mode = blescan_running() ? RMODE_SNIFF : RMODE_IDLE;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "mode=%s (scan win=%ums/%ums)", radio_mode_str(s_mode),
             (unsigned)sc.win_ms, (unsigned)sc.itvl_ms);
    applog_add(LOG_T_SCAN, 0, "mode=sniff %s", e == ESP_OK ? "ok" : "fail");
    return e;
}

esp_err_t radio_clone(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* Exclusive: fully stop scanning first (also avoids capturing our own packets) */
    blescan_stop();

    cfg_lock();
    beacon_cfg_t snap = *cfg_get();      /* ~700 B copy; never do RF work under the lock */
    cfg_unlock();

    esp_err_t e = beacon_adv_apply(&snap);
    bool on = beacon_adv_is_running();
    s_mode = on ? RMODE_CLONE : RMODE_IDLE;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "mode=%s (%s)", radio_mode_str(s_mode), esp_err_to_name(e));
    applog_add(LOG_T_ACT, 0, "mode=clone %s", on ? "on" : "off");
    return e;
}

void radio_on_ble_ready(void)
{
    cfg_lock();
    bool scan_en = cfg_get()->scan.enabled;
    int  active  = cfg_get()->active;
    cfg_unlock();

    /* Default to sniffing (capture first, then clone); clone directly if scanning is off */
    if (scan_en)                 radio_sniff();
    else if (active >= 0)        radio_clone();
    else                         radio_stop();
}