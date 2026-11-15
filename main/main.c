#include "app_cfg.h"
#include "app_log.h"
#include "beacon_adv.h"
#include "beacon_scan.h"
#include "radio.h"
#include "storage.h"
#include "net_ap.h"
#include "web_server.h"
#include "status_led.h"
#include "btn.h"
#include "board.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";     /* supply is too weak */
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}
static volatile bool s_ble_synced;

static void update_led(void)
{
    cfg_lock();
    int count = cfg_get()->count;
    cfg_unlock();

    switch (radio_mode()) {
    case RMODE_CLONE:
        /* Green when somebody is on the web UI, so the field operator can tell */
        status_led_set(net_ap_sta_count() > 0 ? LED_ADV_CLIENT : LED_ADV);
        break;
    case RMODE_SNIFF:
        status_led_set(LED_SNIFF);
        break;
    default:
        status_led_set(count == 0 ? LED_NO_CFG : LED_IDLE);
        break;
    }
}

/* Re-enter clone mode with the current active profile (arbitrated by radio.c) */
esp_err_t app_apply_active(void)
{
    if (!s_ble_synced) return ESP_OK;      /* not synced yet; radio_on_ble_ready() will do it */
    esp_err_t e = radio_clone();
    if (e != ESP_OK) status_led_set(LED_ERROR);
    update_led();
    return e;
}

/* ---------------- button: single = toggle mode, long = factory reset ---------------- */

/* Single click: clone on / off. Scanning is manual-only (from the management page), so the
 * button no longer starts a scan -- a stray click must not put the radio into sniff. */
static void on_btn_short(void)
{
    if (radio_mode() == RMODE_CLONE) {
        radio_stop();
    } else {
        /* With no active profile, fall back to the first enabled one */
        cfg_lock();
        if (cfg_get()->active < 0) {
            for (int i = 0; i < cfg_get()->count; i++) {
                if (cfg_get()->p[i].enabled) { cfg_get()->active = i; cfg_save(); break; }
            }
        }
        cfg_unlock();
        radio_clone();
    }
    update_led();
    applog_add(LOG_T_ACT, 0, "btn 1x: mode=%s", radio_mode_str(radio_mode()));
}

/* Double click: stop the radio, keep the web UI */
static void on_btn_double(void)
{
    radio_stop();
    update_led();
    applog_add(LOG_T_ACT, 0, "btn 2x: mode=idle");
}

static void reboot_cb(void *arg) { esp_restart(); }

static void on_btn_long(void)
{
    ESP_LOGW(TAG, "long press -> factory reset");
    applog_add(LOG_T_CFG, 0, "factory reset by button");
    cfg_factory_reset();
    app_apply_active();

    esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "rst" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 800000);
}

void app_on_ble_sync(void)
{
    s_ble_synced = true;
    radio_on_ble_ready();        /* default to sniffing, waiting for a signal to copy */
    applog_add(LOG_T_ACT, 0, "ble ready -> mode=%s", radio_mode_str(radio_mode()));
    update_led();
}

/* 1 s tick: optional rotation, plus advertising self-heal */
static uint32_t s_rot_tick;
static uint8_t  s_adv_retry;

static void tick_cb(void *arg)
{
    bool apply = false;

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    int enabled_n = 0;
    bool any_public = false;
    for (int i = 0; i < c->count; i++) {
        if (c->p[i].enabled) enabled_n++;
        if (c->p[i].clone_public_addr) any_public = true;
    }

    /* Rotation cannot mix profiles that clone a public address: that address is fixed for
     * the whole boot, so rotating would need a reboot per step. */
    if (c->rotation && any_public && c->active >= 0 && enabled_n > 1 && radio_mode() == RMODE_CLONE) {
        static bool warned;
        if (!warned) { warned = true; ESP_LOGW(TAG, "rotation paused: a profile clones a public address"); }
    }
    if (c->rotation && !any_public && c->active >= 0 && enabled_n > 1 && radio_mode() == RMODE_CLONE) {
        if (++s_rot_tick >= c->rotation_s) {
            s_rot_tick = 0;
            for (int k = 1; k <= c->count; k++) {
                int i = (c->active + k) % c->count;
                if (c->p[i].enabled) {
                    c->active = i;
                    apply = true;
                    break;
                }
            }
            if (apply) cfg_save();
        }
    } else {
        s_rot_tick = 0;
    }
    int newid = c->active;
    cfg_unlock();

    if (apply) {
        app_apply_active();
        applog_add(LOG_T_ACT, 0, "rotation -> %d", newid);
    }

    /* Self-heal: the controller can drop advertising (ADV_COMPLETE, RF error) while
     * the host still believes it is in clone mode. Without this the UI keeps saying
     * "cloning" with ADV off and nothing ever restarts the advertisement. */
    if (radio_mode() == RMODE_CLONE && !beacon_adv_is_running()) {
        if (++s_adv_retry >= 10) {
            s_adv_retry = 0;
            ESP_LOGW(TAG, "advertising dropped in clone mode -> re-apply");
            applog_add(LOG_T_ERR, 0, "adv dropped in clone mode, re-applying");
            app_apply_active();
        }
    } else {
        s_adv_retry = 0;
    }

    update_led();
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(cfg_init());
    applog_init();

    esp_reset_reason_t rr = esp_reset_reason();
    cfg_stat_bump("boots", 1);
    if (rr == ESP_RST_BROWNOUT) cfg_stat_bump("brownout", 1);
    ESP_LOGW(TAG, "board=%s reset=%s boots=%u brownouts=%u",
             BOARD_NAME, reset_reason_str(rr),
             (unsigned)cfg_stat_get("boots"), (unsigned)cfg_stat_get("brownout"));
    applog_add(LOG_T_BOOT, 0, "boot v%s reset=%s", APP_VER, reset_reason_str(rr));
    if (rr == ESP_RST_BROWNOUT) {
        applog_add(LOG_T_ERR, 0, "BROWNOUT: weak supply (short cable / add bulk cap / better PSU)");
    }

    /* BLE public address override (cloning a device that used a PUBLIC address). RAM only and
     * only possible before any network interface is initialised, so it is done once here and
     * a profile switch that needs another address triggers a reboot (radio_clone()). */
    cfg_lock();
    {
        const beacon_cfg_t *c0 = cfg_get();
        if (c0->active >= 0 && c0->active < c0->count && c0->p[c0->active].clone_public_addr) {
            /* note the argument order: (mac, type) */
            esp_err_t ae = esp_iface_mac_addr_set(c0->p[c0->active].src_addr, ESP_MAC_BT);
            ESP_LOGW(TAG, "ble public addr <- " MACSTR " (%s)",
                     MAC2STR(c0->p[c0->active].src_addr), esp_err_to_name(ae));
        }
    }
    cfg_unlock();
    {
        uint8_t bmac[6] = {0};
        if (esp_read_mac(bmac, ESP_MAC_BT) == ESP_OK) cfg_note_boot_addr(bmac);
    }

    ESP_ERROR_CHECK(blescan_init());
    ESP_ERROR_CHECK(radio_init());
    status_led_init();
    btn_init(on_btn_short, on_btn_double, on_btn_long);
    ESP_ERROR_CHECK(net_ap_start());       /* Wi-Fi first; coexistence params latch at BLE init */
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(beacon_stack_init());  /* async: sync_cb -> app_on_ble_sync */

    const esp_timer_create_args_t ta = { .callback = tick_cb, .name = "tick" };
    esp_timer_handle_t tick;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 1000000));

    cfg_lock();
    ESP_LOGI(TAG, "ready: %d profile(s), active=%d, ap='%s', http://192.168.4.1/",
             cfg_get()->count, cfg_get()->active, cfg_get()->ap.ssid);
    cfg_unlock();
}
