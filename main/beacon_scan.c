#include "beacon_scan.h"
#include "app_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"

static const char *TAG = "scan";

static detection_t       s_tab[SCAN_TAB_MAX];
static int               s_used;
static SemaphoreHandle_t s_mtx;
static volatile bool     s_running;
static uint16_t          s_itvl_ms = 300, s_win_ms = 60;
static uint32_t          s_pkts;        /* 累计收到的广播包 */
static uint32_t          s_since_ms;    /* 本次扫描开始时间 */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 解析 AD 结构；对非 iBeacon 包也会填 name / adv_len */
static void parse_adv(const uint8_t *d, uint8_t len, detection_t *r)
{
    uint8_t i = 0;
    r->is_ibeacon = false;
    while (i + 1 < len) {
        uint8_t l = d[i];
        if (l == 0 || (int)i + 1 + l > (int)len) break;
        uint8_t type = d[i + 1];
        const uint8_t *v = &d[i + 2];
        if (type == 0xFF && l >= 26) {
            /* iBeacon: 4C 00 | 02 15 | uuid16 | major2 | minor2 | ref_rssi */
            if (v[0] == 0x4C && v[1] == 0x00 && v[2] == 0x02 && v[3] == 0x15) {
                memcpy(r->uuid, &v[4], 16);
                r->major    = (uint16_t)((v[20] << 8) | v[21]);
                r->minor    = (uint16_t)((v[22] << 8) | v[23]);
                r->ref_rssi = (int8_t)v[24];
                r->is_ibeacon = true;
            }
        } else if ((type == 0x08 || type == 0x09) && l >= 2) {
            int n = l - 1;
            if (n > (int)sizeof(r->name) - 1) n = sizeof(r->name) - 1;
            memcpy(r->name, v, (size_t)n);
            r->name[n] = '\0';
        }
        i = (uint8_t)(i + l + 1);
    }
}

static void aggregate(const struct ble_gap_disc_desc *desc)
{
    uint32_t t = now_ms();
    bool     do_log = false;
    bool     was_new = false;
    detection_t snap = {0};

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pkts++;

    detection_t *slot = NULL;
    for (int i = 0; i < s_used; i++) {
        if (s_tab[i].addr_type == desc->addr.type &&
            memcmp(s_tab[i].addr, desc->addr.val, 6) == 0) {
            slot = &s_tab[i];
            break;
        }
    }
    if (!slot) {
        if (s_used < SCAN_TAB_MAX) {
            slot = &s_tab[s_used++];
        } else {                       /* LRU 淘汰最久未见 */
            int oldest = 0;
            for (int i = 1; i < SCAN_TAB_MAX; i++)
                if (s_tab[i].last_ms < s_tab[oldest].last_ms) oldest = i;
            slot = &s_tab[oldest];
        }
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->addr, desc->addr.val, 6);
        slot->addr_type = desc->addr.type;
        slot->adv_type  = desc->event_type;
        slot->first_ms  = t;
        slot->rssi_max  = desc->rssi;
        slot->rssi_avg  = desc->rssi;
        was_new = true;
    }

    /* 估计广播间隔：用相邻包间隔做 EMA（只吃 20ms..10s 的合理值） */
    if (slot->count > 0) {
        uint32_t gap = t - slot->last_ms;
        if (gap >= 20 && gap <= 10000) {
            slot->itvl_ms = slot->itvl_ms ? (uint16_t)((slot->itvl_ms * 3u + gap) / 4u)
                                          : (uint16_t)gap;
        }
    }

    slot->count++;
    slot->last_ms = t;
    slot->rssi_last = desc->rssi;
    if (desc->rssi > slot->rssi_max) slot->rssi_max = desc->rssi;
    slot->rssi_avg = (int8_t)((slot->rssi_avg * 7 + desc->rssi) / 8);   /* IIR 平滑 */

    /* 原始字节：复刻时逐字节照搬，不依赖上层解析 */
    uint8_t n = desc->length_data;
    if (n > SCAN_RAW_MAX) n = SCAN_RAW_MAX;
    memcpy(slot->raw, desc->data, n);
    slot->raw_len = n;

    parse_adv(desc->data, desc->length_data, slot);

    /* 同一 MAC 5 分钟内只记一条日志，避免日志环被刷爆 */
    if (was_new || (t - slot->last_log_ms) > 300000u) {
        slot->last_log_ms = t;
        do_log = true;
        snap = *slot;
    }

    xSemaphoreGive(s_mtx);

    if (do_log) {
        if (snap.is_ibeacon) {
            char u[33];
            cfg_uuid_to_str(snap.uuid, u);
            applog_add(LOG_T_DETECT, snap.rssi_last, "%02x%02x%02x%02x%02x%02x ibeacon %u/%u rssi%d",
                       snap.addr[0], snap.addr[1], snap.addr[2],
                       snap.addr[3], snap.addr[4], snap.addr[5],
                       snap.major, snap.minor, snap.rssi_last);
        } else {
            applog_add(LOG_T_DETECT, snap.rssi_last, "%02x%02x%02x%02x%02x%02x raw%d '%s' rssi%d",
                       snap.addr[0], snap.addr[1], snap.addr[2],
                       snap.addr[3], snap.addr[4], snap.addr[5],
                       snap.raw_len, snap.name[0] ? snap.name : "?", snap.rssi_last);
        }
    }
}

static int disc_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        aggregate(&ev->disc);          /* 只做解析+入表，禁止 IO */
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_running = false;
        ESP_LOGW(TAG, "scan completed, reason=%d", ev->disc_complete.reason);
        break;
    default:
        break;
    }
    return 0;
}

esp_err_t blescan_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx ? ESP_OK : ESP_ERR_NO_MEM;
}

bool blescan_running(void) { return s_running; }

void blescan_stop(void)
{
    if (!s_mtx) return;
    if (s_running) {
        ble_gap_disc_cancel();
        s_running = false;
    }
}

esp_err_t blescan_start(const scan_cfg_t *sc)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    blescan_stop();
    if (!sc->enabled) {
        ESP_LOGI(TAG, "scan disabled");
        return ESP_OK;
    }

    uint32_t itvl = sc->itvl_ms < 20 ? 20 : sc->itvl_ms;
    uint32_t win  = sc->win_ms  < 2  ? 2  : sc->win_ms;
    if (win > itvl) win = itvl;

    /* 捕获与广播互斥，占空比上限放宽到 50%（再高会把 SoftAP 饿死） */
    uint32_t cap = itvl / 2;
    if (cap < 2) cap = 2;
    if (win > cap) {
        ESP_LOGW(TAG, "scan duty %.0f%% too high -> capped 50%% (win=%ums)",
                 (double)win * 100.0 / (double)itvl, (unsigned)cap);
        win = cap;
    }
    s_itvl_ms = (uint16_t)itvl;
    s_win_ms  = (uint16_t)win;

    uint32_t u_itvl = itvl * 8 / 5;                 /* ms -> 0.625ms 单位 */
    uint32_t u_win  = win * 8 / 5;
    if (u_itvl < 4) u_itvl = 4;
    if (u_itvl > 0x4000) u_itvl = 0x4000;
    if (u_win < 4) u_win = 4;
    if (u_win > u_itvl) u_win = u_itvl;

    struct ble_gap_disc_params p = {0};
    p.itvl              = (uint16_t)u_itvl;
    p.window            = (uint16_t)u_win;
    p.passive           = 1;        /* 被动扫描：不发 SCAN_REQ，最省电 */
    p.filter_duplicates = 0;        /* 自己做聚合，控制器去重会吃掉计数 */

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, disc_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        applog_add(LOG_T_ERR, 0, "disc start rc=%d", rc);
        return ESP_FAIL;
    }
    s_running = true;
    s_since_ms = now_ms();
    s_pkts = 0;
    ESP_LOGI(TAG, "scan on: win=%ums itvl=%ums (%.0f%%)",
             (unsigned)s_win_ms, (unsigned)s_itvl_ms,
             (double)s_win_ms * 100.0 / (double)s_itvl_ms);
    return ESP_OK;
}

void blescan_stats(uint32_t *pkts, uint32_t *running_ms)
{
    if (pkts) *pkts = s_pkts;
    if (running_ms) *running_ms = s_running ? (now_ms() - s_since_ms) : 0;
}

int blescan_count(void)
{
    int n;
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    n = s_used;
    xSemaphoreGive(s_mtx);
    return n;
}

int blescan_snapshot(detection_t *out, int max)
{
    if (!s_mtx || !out) return 0;
    int n = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    n = s_used < max ? s_used : max;
    memcpy(out, s_tab, sizeof(detection_t) * (size_t)n);
    xSemaphoreGive(s_mtx);

    /* 按最后出现时间倒序（插入排序，n<=32） */
    for (int i = 1; i < n; i++) {
        detection_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].last_ms < k.last_ms) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return n;
}

bool blescan_find(const uint8_t addr[6], detection_t *out)
{
    bool found = false;
    if (!s_mtx || !addr || !out) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < s_used; i++) {
        if (memcmp(s_tab[i].addr, addr, 6) == 0) { *out = s_tab[i]; found = true; break; }
    }
    xSemaphoreGive(s_mtx);
    return found;
}

void blescan_clear(void)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(s_tab, 0, sizeof(s_tab));
    s_used = 0;
    xSemaphoreGive(s_mtx);
}
