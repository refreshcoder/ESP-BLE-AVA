#include "radio.h"
#include "app_cfg.h"
#include "app_log.h"
#include "beacon_adv.h"
#include "beacon_scan.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "radio";

#define SNIFF_TIMEOUT_US (30 * 1000 * 1000)   /* manual sniff stops by itself */

static SemaphoreHandle_t s_mtx;
static volatile rmode_t  s_mode = RMODE_IDLE;
static int64_t           s_sniff_deadline;      /* 0 = not sniffing */

static void addr_reboot_cb(void *arg) { esp_restart(); }

/* Persist the mode so a reboot (e.g. to apply a new public address) resumes it instead of
 * falling back to sniffing. Mode changes are user-driven, so one NVS write each is fine. */
static void remember_mode(rmode_t m)
{
    uint8_t v = m == RMODE_CLONE ? CFG_MODE_CLONE
              : m == RMODE_SNIFF ? CFG_MODE_SNIFF : CFG_MODE_IDLE;
    cfg_lock();
    if (cfg_get()->last_mode != v) {
        cfg_get()->last_mode = v;
        cfg_save();
    }
    cfg_unlock();
}

const char *radio_mode_str(rmode_t m)
{
    switch (m) {
    case RMODE_SNIFF: return "sniff";
    case RMODE_CLONE: return "clone";
    default:          return "idle";
    }
}

rmode_t radio_mode(void) { return s_mode; }

/* Scanning is started by hand, so it must not keep running forever. */
bool radio_sniff_timed_out(void)
{
    return s_sniff_deadline != 0 && esp_timer_get_time() >= s_sniff_deadline;
}

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
    s_sniff_deadline = 0;
    xSemaphoreGive(s_mtx);
    remember_mode(s_mode);
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
    s_sniff_deadline = (s_mode == RMODE_SNIFF) ? esp_timer_get_time() + SNIFF_TIMEOUT_US : 0;
    xSemaphoreGive(s_mtx);
    remember_mode(s_mode);

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
    s_sniff_deadline = 0;
    xSemaphoreGive(s_mtx);
    remember_mode(s_mode);

    ESP_LOGI(TAG, "mode=%s (%s)", radio_mode_str(s_mode), esp_err_to_name(e));
    applog_add(LOG_T_ACT, 0, "mode=clone %s", on ? "on" : "off");

    /* The BLE public address can only be set before the stack is initialised, so a profile
     * that needs a different one takes effect after a reboot. 1.2 s leaves room for the
     * HTTP response / log line to go out first. */
    if (on && cfg_addr_change_pending()) {
        ESP_LOGW(TAG, "active profile needs another public address -> rebooting");
        applog_add(LOG_T_CFG, 0, "public addr change -> reboot");
        esp_timer_handle_t t;
        const esp_timer_create_args_t a = { .callback = addr_reboot_cb, .name = "addr_rst" };
        if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 1200000);
    }
    return e;
}

void radio_on_ble_ready(void)
{
    cfg_lock();
    int  active  = cfg_get()->active;
    uint8_t last = cfg_get()->last_mode;
    cfg_unlock();

    /* Manual sniffing: a boot restores "broadcasting" or "parked with the radio off".
     * Scanning is only ever started from the management page (or the button), never
     * automatically -- so last_mode == SNIFF simply means "idle" after a reboot. */
    if (last == CFG_MODE_CLONE && active >= 0)     radio_clone();
    else                                           radio_stop();
}