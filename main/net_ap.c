#include "net_ap.h"
#include "app_cfg.h"
#include "app_log.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"

static const char *TAG = "ap";
static esp_netif_t *s_netif;
static volatile bool s_up;
static volatile uint8_t s_sta;

static void wifi_ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            s_up = true;
            ESP_LOGI(TAG, "softap started");
            applog_add(LOG_T_BOOT, 0, "softap up");
            break;
        case WIFI_EVENT_AP_STOP:
            s_up = false;
            ESP_LOGW(TAG, "softap stopped");
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            if (s_sta < 255) s_sta++;
            ESP_LOGI(TAG, "sta join " MACSTR ", aid=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            if (s_sta) s_sta--;
            ESP_LOGI(TAG, "sta leave " MACSTR ", aid=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        default:
            break;
        }
    }
}

esp_err_t net_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) return ESP_FAIL;

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev, NULL, NULL));

    cfg_lock();
    char ssid[CFG_SSID_MAX + 1], pass[CFG_PASS_MAX + 1];
    strncpy(ssid, cfg_get()->ap.ssid, sizeof(ssid) - 1); ssid[sizeof(ssid) - 1] = 0;
    strncpy(pass, cfg_get()->ap.pass, sizeof(pass) - 1); pass[sizeof(pass) - 1] = 0;
    cfg_unlock();

    wifi_config_t c = {0};
    memcpy(c.ap.ssid, ssid, strlen(ssid));
    c.ap.ssid_len       = (uint8_t)strlen(ssid);
    memcpy(c.ap.password, pass, strlen(pass));
    c.ap.channel        = 6;
    c.ap.max_connection = 4;
    c.ap.authmode       = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &c));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 省电模式对小流量管理页无收益，关掉可降低延迟 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "SSID='%s' auth=%d", ssid, c.ap.authmode);
    return ESP_OK;
}

bool         net_ap_is_up(void)   { return s_up; }
uint8_t      net_ap_sta_count(void) { return s_sta; }
esp_netif_t *net_ap_netif(void)   { return s_netif; }

esp_err_t net_ap_set_creds(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) < 1 || strlen(ssid) > CFG_SSID_MAX) return ESP_ERR_INVALID_ARG;
    if (!pass || strlen(pass) > CFG_PASS_MAX) return ESP_ERR_INVALID_ARG;
    if (strlen(pass) > 0 && strlen(pass) < 8) return ESP_ERR_INVALID_ARG;   /* WPA2 最短 8 位 */

    cfg_lock();
    strncpy(cfg_get()->ap.ssid, ssid, CFG_SSID_MAX);
    cfg_get()->ap.ssid[CFG_SSID_MAX] = '\0';
    strncpy(cfg_get()->ap.pass, pass, CFG_PASS_MAX);
    cfg_get()->ap.pass[CFG_PASS_MAX] = '\0';
    cfg_save();
    cfg_unlock();

    wifi_config_t c = {0};
    memcpy(c.ap.ssid, ssid, strlen(ssid));
    c.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(c.ap.password, pass, strlen(pass));
    c.ap.channel = 6;
    c.ap.max_connection = 4;
    c.ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &c);
    ESP_LOGI(TAG, "creds updated: %s", esp_err_to_name(e));
    applog_add(LOG_T_CFG, 0, "ap creds changed");
    return e;
}
