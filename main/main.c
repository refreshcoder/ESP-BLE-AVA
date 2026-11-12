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
    case ESP_RST_BROWNOUT: return "brownout";     /* ← 供电不足 */
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
        /* 有人连着管理页时用绿色区分，方便现场判断"是不是有人在动配置" */
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

/* 按当前 active 配置重新进入复刻模式（受 radio 互斥保护） */
esp_err_t app_apply_active(void)
{
    if (!s_ble_synced) return ESP_OK;      /* 还没同步，等 radio_on_ble_ready 再进模式 */
    esp_err_t e = radio_clone();
    if (e != ESP_OK) status_led_set(LED_ERROR);
    update_led();
    return e;
}

/* ---------------- 按键：短按切广播 / 长按恢复出厂 ---------------- */

/* 单击：监听 ⇄ 复刻 切换（现场最常用的动作） */
static void on_btn_short(void)
{
    if (radio_mode() == RMODE_CLONE) {
        radio_sniff();
    } else {
        /* 没有 active 配置时退化为选择第一个可用配置 */
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

/* 双击：停止射频（只留 AP 管理页） */
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
    radio_on_ble_ready();        /* 默认进监听模式，等着被抓的信号 */
    applog_add(LOG_T_ACT, 0, "ble ready -> mode=%s", radio_mode_str(radio_mode()));
    update_led();
}

/* 1s tick：自动轮换 */
static uint32_t s_rot_tick;

static void tick_cb(void *arg)
{
    bool apply = false;

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    int enabled_n = 0;
    for (int i = 0; i < c->count; i++) if (c->p[i].enabled) enabled_n++;

    if (c->rotation && c->active >= 0 && enabled_n > 1 && radio_mode() == RMODE_CLONE) {
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
        applog_add(LOG_T_ERR, 0, "BROWNOUT: 供电不足(换短线/加电容/换电源)");
    }

    ESP_ERROR_CHECK(blescan_init());
    ESP_ERROR_CHECK(radio_init());
    status_led_init();
    btn_init(on_btn_short, on_btn_double, on_btn_long);
    ESP_ERROR_CHECK(net_ap_start());       /* WiFi 先起，共存参数在 BLE 初始化时生效 */
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(beacon_stack_init());  /* 异步：sync 后回调 app_on_ble_sync */

    const esp_timer_create_args_t ta = { .callback = tick_cb, .name = "tick" };
    esp_timer_handle_t tick;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 1000000));

    cfg_lock();
    ESP_LOGI(TAG, "ready: %d profile(s), active=%d, ap='%s', http://192.168.4.1/",
             cfg_get()->count, cfg_get()->active, cfg_get()->ap.ssid);
    cfg_unlock();
}
