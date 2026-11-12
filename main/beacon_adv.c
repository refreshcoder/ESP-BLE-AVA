#include "beacon_adv.h"
#include "app_log.h"
#include "board.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "adv";

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool    s_synced;
static volatile bool s_running;
static uint32_t s_starts;        /* number of times advertising was started */
static int8_t   s_tx_dbm = 9;    /* last successfully applied TX power */

/* iBeacon manufacturer data, excluding the AD length/type bytes: 25 bytes
   4C 00 | 02 15 | UUID(16) | major(2) | minor(2) | ref_rssi(1) */
static uint8_t s_mfg[25];

static void snap_tx_power(int8_t dbm)
{
    int v = dbm < BOARD_BLE_TX_MIN_DBM ? BOARD_BLE_TX_MIN_DBM
          : dbm > BOARD_BLE_TX_MAX_DBM ? BOARD_BLE_TX_MAX_DBM : dbm;
    if (v >= 0) v = ((v + 1) / 3) * 3;
    else        v = -(((-v) + 1) / 3) * 3;
    esp_err_t e = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)v);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "tx_power %d dBm: %s", v, esp_err_to_name(e));
    } else {
        s_tx_dbm = (int8_t)v;
    }
}

static void build_mfg(const beacon_profile_t *p)
{
    s_mfg[0] = 0x4C; s_mfg[1] = 0x00;      /* Apple Company ID */
    s_mfg[2] = 0x02; s_mfg[3] = 0x15;      /* beacon, 21 bytes */
    memcpy(&s_mfg[4], p->uuid, CFG_UUID_LEN);
    s_mfg[20] = (uint8_t)(p->major >> 8);
    s_mfg[21] = (uint8_t)(p->major & 0xFF);
    s_mfg[22] = (uint8_t)(p->minor >> 8);
    s_mfg[23] = (uint8_t)(p->minor & 0xFF);
    s_mfg[24] = (uint8_t)p->ref_rssi;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_running = false;
        ESP_LOGW(TAG, "advertising completed, reason=%d", ev->adv_complete.reason);
        applog_add(LOG_T_ERR, 0, "adv complete reason=%d", ev->adv_complete.reason);
        break;
    default:
        break;
    }
    return 0;
}

void beacon_adv_stop(void)
{
    if (!s_synced) { s_running = false; return; }
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_stop rc=%d", rc);
    }
    s_running = false;
}

bool beacon_adv_is_running(void) { return s_running; }

void beacon_adv_stats(uint32_t *starts, int8_t *tx_dbm)
{
    if (starts) *starts = s_starts;
    if (tx_dbm) *tx_dbm = s_tx_dbm;
}

esp_err_t beacon_adv_apply(const beacon_cfg_t *c)
{
    if (!s_synced) return ESP_ERR_INVALID_STATE;

    /* 1) Must stop first: changing fields while advertising returns BLE_HS_EBUSY */
    beacon_adv_stop();

    if (c->active < 0 || c->active >= c->count) {
        ESP_LOGI(TAG, "no active profile -> broadcasting off");
        return ESP_OK;
    }
    const beacon_profile_t *p = &c->p[c->active];
    if (!p->enabled) {
        ESP_LOGW(TAG, "profile %d disabled -> off", c->active);
        return ESP_OK;
    }

    /* 2) TX power + payload */
    snap_tx_power(p->radio_dbm);

    int rc = 0;
    if (p->kind == PROFILE_KIND_RAW) {
        /* Verbatim replay: the whole AD structure is copied byte for byte, no re-encoding */
        if (p->raw_len < 2 || p->raw_len > 31) {
            ESP_LOGE(TAG, "raw_len %u invalid (2..31)", p->raw_len);
            return ESP_ERR_INVALID_ARG;
        }
        rc = ble_gap_adv_set_data(p->raw, p->raw_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "adv_set_data rc=%d", rc);
            applog_add(LOG_T_ERR, 0, "set_data rc=%d", rc);
            return ESP_FAIL;
        }
    } else {
        build_mfg(p);
        struct ble_hs_adv_fields f = {0};
        f.flags        = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        f.mfg_data     = s_mfg;
        f.mfg_data_len = sizeof(s_mfg);
        rc = ble_gap_adv_set_fields(&f);
        if (rc != 0) {
            ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
            applog_add(LOG_T_ERR, 0, "set_fields rc=%d", rc);
            return ESP_FAIL;
        }
    }

    /* 3) Address: optional static-random, one per profile, so downstream can tell devices apart */
    uint8_t own = s_own_addr_type;
    if (p->use_rand_addr) {
        uint8_t a[6];
        memcpy(a, p->rand_addr, 6);
        a[5] |= 0xC0;                       /* top two bits = 11: static random */
        if (ble_hs_id_set_rnd(a) == 0) own = BLE_OWN_ADDR_RANDOM;
        else ESP_LOGW(TAG, "set_rnd failed, fallback to public");
    }

    /* 4) Advertising parameters; interval unit is 0.625 ms */
    uint32_t itvl = (uint32_t)p->adv_itvl_ms * 8 / 5;
    if (itvl < 0x20)  itvl = 0x20;          /* 20ms */
    if (itvl > 0x4000) itvl = 0x4000;       /* 10240ms */

    struct ble_gap_adv_params ap = {0};
    ap.conn_mode   = BLE_GAP_CONN_MODE_NON;
    ap.disc_mode   = p->scannable ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON;
    ap.itvl_min    = (uint16_t)itvl;
    ap.itvl_max    = (uint16_t)itvl;
    ap.channel_map = 0x07;                  /* channels 37/38/39 */

    rc = ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        applog_add(LOG_T_ERR, 0, "adv_start rc=%d", rc);
        return ESP_FAIL;
    }
    s_running = true;
    s_starts++;

    if (p->kind == PROFILE_KIND_RAW) {
        char h[CFG_RAW_MAX * 2 + 1];
        cfg_hex_to_str(p->raw, p->raw_len, h);
        ESP_LOGI(TAG, "clone on: '%s' raw=%s itvl=%ums tx=%ddBm%s",
                 p->name, h, p->adv_itvl_ms, p->radio_dbm,
                 p->scannable ? " (scannable)" : "");
    } else {
        char u[33];
        cfg_uuid_to_str(p->uuid, u);
        ESP_LOGI(TAG, "adv on: '%s' uuid=%s %u/%u ref=%d itvl=%ums tx=%ddBm",
                 p->name, u, p->major, p->minor, p->ref_rssi, p->adv_itvl_ms, p->radio_dbm);
    }
    return ESP_OK;
}

/* ---------------- BLE stack ---------------- */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0) {
        ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    s_synced = true;
    ESP_LOGI(TAG, "host synced, own_addr_type=%d", s_own_addr_type);
    app_on_ble_sync();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
    s_synced  = false;
    s_running = false;
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t beacon_stack_init(void)
{
    esp_err_t e = nimble_port_init();          /* controller + HCI init (IDF 5.x) */
    if (e != ESP_OK) return e;

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* Note: the GAP/GATT services are only compiled into libbt.a when
     * CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y. This project is broadcaster + observer
     * only and never accepts connections, so no GAP service is initialised. */
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
