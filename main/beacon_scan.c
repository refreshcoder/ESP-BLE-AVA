#include "beacon_scan.h"
#include "app_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"

static const char *TAG = "scan";

/* BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP; kept numeric so the constant name cannot differ
 * between NimBLE versions. */
#define ADV_RPT_SCAN_RSP 0x04

static detection_t       s_tab[SCAN_TAB_MAX];
static int               s_used;
static SemaphoreHandle_t s_mtx;
static volatile bool     s_running;
static uint16_t          s_itvl_ms = 300, s_win_ms = 60;
static uint32_t          s_pkts;        /* advertising packets received */
static uint32_t          s_filtered;    /* ... of which dropped as personal-device noise */
static bool              s_filter_noise = true;
static uint32_t          s_since_ms;    /* scan start timestamp */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Walk the AD structures; fills name/adv_len for non-iBeacon packets too */
/* Walk the AD structures and keep the first manufacturer specific block.
 * Layout on air: len | 0xFF | company id (2, little-endian) | payload...
 * iBeacon is just a special case of this (company 0x004C), parsed separately. */
static void parse_mfg(const uint8_t *d, uint8_t len, detection_t *r, uint8_t src)
{
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t l = d[i];
        if (l == 0 || (int)i + 1 + l > (int)len) break;
        if (d[i + 1] == 0xFF && l >= 3) {
            const uint8_t *v = &d[i + 2];
            r->mfg_company = (uint16_t)(v[0] | (v[1] << 8));
            int n = l - 3;                       /* minus type byte and company id */
            if (n > (int)sizeof(r->mfg)) n = sizeof(r->mfg);
            memcpy(r->mfg, &v[2], (size_t)n);
            r->mfg_len = (uint8_t)n;
            r->mfg_src = src;
            return;                              /* first one wins */
        }
        i = (uint8_t)(i + l + 1);
    }
}

/* Company ids below are the SIG "Company Identifier" codes (little-endian on air).
 * Deliberately short labels: they are drawn in a fixed-width column of the UI. */
typedef struct { uint16_t id; const char *name; } id_name_t;

static const id_name_t k_company[] = {
    { 0x004C, "Apple" },
    { 0x0006, "Microsoft" },
    { 0x0075, "Samsung" },
    { 0x00E0, "Google" },
    { 0x0059, "Nordic" },
    { 0x027D, "Huawei" },
    { 0x038F, "Xiaomi" },
    /* DingTalk / Alipay / Tmall Genie devices all report under Alibaba's code */
    { 0x0EC5, "Alibaba" },
    { 0x013A, "Tencent" },
    { 0x079A, "OPPO" },
    { 0x0837, "vivo" },
    { 0x072F, "OnePlus" },
    { 0x03AB, "Meizu" },
    { 0x02C5, "Lenovo" },
    { 0x0958, "ZTE" },
    { 0x0BC6, "TCL" },
    /* appliances */
    { 0x06A8, "Midea" },
    { 0x0929, "Haier" },
    { 0x0D23, "Gree" },
    { 0x0F1F, "Ninebot" },
    { 0x08AA, "DJI" },
    { 0x0CC2, "Anker" },
    { 0x0310, "SGL Italia" },
    /* access control / attendance (see docs/attendance-vendors.md) */
    { 0x0100, "TomTom" },        /* DingTalk's BLE check-in terminals advertise under this id */
    { 0x0E25, "Hikvision" },
    { 0x0124, "HID Global" },
    { 0x013B, "Allegion" },
    { 0x0C64, "dormakaba" },
    { 0x0199, "SALTO" },
    { 0x0B5D, "Newland" },
    { 0x01F1, "Zebra" },
    { 0x0526, "Honeywell" },
    { 0x0E60, "Ant Group" },
    { 0x011C, "Baidu" },
    { 0x0025, "NXP" },
    /* wearables + trackers */
    { 0x0157, "Huami" },
    { 0x0087, "Garmin" },
    { 0x006B, "Polar" },
    { 0x009F, "Suunto" },
    { 0x03FF, "Withings" },
    { 0x0304, "Oura" },
    { 0x067C, "Tile" },
    { 0x08C3, "Chipolo" },
    { 0x015D, "Estimote" },
    { 0x01FD, "Kontakt" },
    /* audio */
    { 0x009E, "Bose" },
    { 0x012D, "Sony" },
    { 0x0494, "Sennheiser" },
    { 0x0057, "Harman" },
    { 0x00CC, "Beats" },
    { 0x01DA, "Logitech" },
    { 0x0055, "Plantronics" },
    /* health */
    { 0x020E, "Omron" },
    { 0x03BB, "Abbott" },
    { 0x00D0, "Dexcom" },
    { 0x01F9, "Medtronic" },
    { 0x0170, "Roche" },
    { 0x0167, "Ascensia" },
    /* silicon: tells you the module, not the product */
    { 0x0002, "Intel" },
    { 0x000D, "TI" },
    { 0x000A, "Qualcomm" },
    { 0x005D, "Realtek" },
    { 0x02E5, "Espressif" },
    { 0x0131, "Cypress" },
    { 0x000F, "Broadcom" },
    { 0x0499, "Ruuvi" },
};

const char *blescan_company_name(uint16_t id)
{
    for (size_t i = 0; i < sizeof(k_company) / sizeof(k_company[0]); i++) {
        if (k_company[i].id == id) return k_company[i].name;
    }
    static char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", id);
    return buf;
}

/* 16-bit service UUIDs worth naming in an advertisement (GATT services are not
 * listed here: they never show up on air unless the device also advertises them). */
static const id_name_t k_service[] = {
    { 0xFCB2, "Location Enabled Adv" },   /* asset trackers / find-my style */
    { 0xFE2C, "Fast Pair" },               /* Google */
    { 0xFD6F, "Exposure Notification" },   /* Apple/Google */
    { 0xFEAA, "Eddystone" },               /* Google */
    { 0xFE59, "Nordic DFU" },
};

const char *blescan_service_name(uint16_t uuid)
{
    for (size_t i = 0; i < sizeof(k_service) / sizeof(k_service[0]); i++) {
        if (k_service[i].id == uuid) return k_service[i].name;
    }
    static char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", uuid);
    return buf;
}

/* Most CN attendance / access-control terminals never advertise a SIG company id, so the
 * local name is the only handle (see docs/attendance-vendors.md). Substring, case
 * insensitive. NULL when nothing matches: the caller must not invent a vendor. */
typedef struct { const char *needle; const char *vendor; } name_hint_t;

static const name_hint_t k_name_hint[] = {
    { "RTK_BT",   "DingTalk" },   /* DingTalk BLE check-in terminal, per field dumps */
    { "ZKTECO",   "ZKTeco" },
    { "ANVIZ",    "Anviz" },
    { "SUPREMA",  "Suprema" },
    { "HIKVISION", "Hikvision" },
    { "DAHUA",    "Dahua" },
    { "HANVON",   "Hanvon" },
    { "SUNMI",    "Sunmi" },
    { "TELPO",    "Telpo" },
};

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) return true;
    }
    return false;
}

const char *blescan_name_vendor(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (size_t i = 0; i < sizeof(k_name_hint) / sizeof(k_name_hint[0]); i++) {
        if (contains_nocase(name, k_name_hint[i].needle)) return k_name_hint[i].vendor;
    }
    return NULL;
}

/* ---------------------- personal-device noise filter ---------------------- */

/* Names of earbuds / wrist wearables: they carry no company id worth keeping and they are
 * exactly what fills a blind scan. Substring, case-insensitive, bounded (no NUL in AD). */
static const char *k_noise_words[] = {
    "buds", "airpods", "freebuds", "edifier", "qcy", "soundcore", "amazfit",
    "mi band", "smart band", "smartwatch", "watch",
};

/* Phone vendors: a hijacked slot for a phone is a slot a beacon cannot use. Only dropped
 * when the payload carries no 16-bit service UUID, so Xiaomi MiBeacon sensors (0xFE95)
 * and friends survive. */
static bool is_phone_vendor(uint16_t cid)
{
    switch (cid) {
    case 0x038F:  /* Xiaomi */      case 0x079A:  /* OPPO */
    case 0x0837:  /* vivo */        case 0x09C6:  /* Honor */
    case 0x027D:  /* Huawei */      case 0x072F:  /* OnePlus */
    case 0x0075:  /* Samsung */     case 0x00E0:  /* Google */
        return true;
    default:
        return false;
    }
}

static bool mem_contains_nocase(const uint8_t *hay, int n, const char *needle)
{
    int nn = (int)strlen(needle);
    for (int i = 0; i + nn <= n; i++) {
        int k = 0;
        while (k < nn && tolower(hay[i + k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nn) return true;
    }
    return false;
}

/* True when this advertisement belongs to a person rather than to the room:
 * Apple Continuity (phones, watches, earbuds, Find My -- but NOT iBeacon),
 * Microsoft CDP / Swift Pair (Windows PCs), earbud/wearable names, and phone vendors
 * that advertise no service at all. */
bool blescan_is_noise(const uint8_t *d, uint8_t len)
{
    if (!d || len < 3) return false;
    bool phone_vendor = false, has_svc = false;
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t l = d[i];
        if (l == 0 || (int)i + 1 + l > (int)len) break;
        uint8_t type = d[i + 1];
        const uint8_t *v = &d[i + 2];
        if (type == 0xFF && l >= 3) {
            uint16_t cid = (uint16_t)(v[0] | (v[1] << 8));
            if (cid == 0x004C) {
                if (l >= 4 && v[2] != 0x02) return true;   /* Apple Continuity, iBeacon kept */
            } else if (cid == 0x0006) {
                if (l >= 6 && v[2] == 0x01 && v[3] == 0x09 && v[4] == 0x20 && v[5] == 0x22)
                    return true;                          /* Microsoft CDP (Windows) */
            } else if (is_phone_vendor(cid)) {
                phone_vendor = true;
            }
        } else if (type == 0x02 || type == 0x03 || type == 0x16) {
            has_svc = true;
        } else if (type == 0x08 || type == 0x09) {
            for (size_t k = 0; k < sizeof(k_noise_words) / sizeof(k_noise_words[0]); k++) {
                if (mem_contains_nocase(v, l - 1, k_noise_words[k])) return true;
            }
        }
        i = (uint8_t)(i + l + 1);
    }
    return phone_vendor && !has_svc;
}

uint32_t blescan_filtered(void) { return s_filtered; }

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
        } else if ((type == 0x02 || type == 0x03) && l >= 3 && r->svc_uuid == 0) {
            /* 16-bit service UUID list: the first entry is enough to classify */
            r->svc_uuid = (uint16_t)(v[0] | (v[1] << 8));
        } else if (type == 0x16 && l >= 3 && r->svc_uuid == 0) {
            /* service data: carries the UUID that identifies the ecosystem */
            r->svc_uuid = (uint16_t)(v[0] | (v[1] << 8));
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
    if (s_filter_noise && blescan_is_noise(desc->data, desc->length_data)) {
        s_filtered++;
        xSemaphoreGive(s_mtx);
        return;
    }

    /* NimBLE keeps the address little-endian (on-air order); everything else in this project
     * (UI, logs, NVS blob, esp_read_mac) uses display order. Convert once, here. */
    uint8_t abe[6];
    for (int k = 0; k < 6; k++) abe[k] = desc->addr.val[5 - k];

    detection_t *slot = NULL;
    for (int i = 0; i < s_used; i++) {
        if (s_tab[i].addr_type == desc->addr.type &&
            memcmp(s_tab[i].addr, abe, 6) == 0) {
            slot = &s_tab[i];
            break;
        }
    }
    if (!slot) {
        /* A scan response can only complete a row: without the advertisement there is
         * nothing to replay, and creating a slot here would leave a zero-filled record
         * (exactly what happened once the noise filter started dropping the ADVs of
         * phones while their scan responses still arrived). */
        if (desc->event_type == ADV_RPT_SCAN_RSP) {
            xSemaphoreGive(s_mtx);
            return;
        }
        if (s_used < SCAN_TAB_MAX) {
            slot = &s_tab[s_used++];
        } else {                       /* LRU: evict the one not seen for longest */
            int oldest = 0;
            for (int i = 1; i < SCAN_TAB_MAX; i++)
                if (s_tab[i].last_ms < s_tab[oldest].last_ms) oldest = i;
            slot = &s_tab[oldest];
        }
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->addr, abe, 6);
        slot->addr_type = desc->addr.type;
        slot->adv_type  = desc->event_type;
        slot->first_ms  = t;
        slot->rssi_max  = desc->rssi;
        slot->rssi_avg  = desc->rssi;
        was_new = true;
    }

    /* A scan response answers our own SCAN_REQ; it is not an advertisement. Store it
     * separately and return, so the replayed ADV payload and the interval estimate are
     * never touched by it. The address matches, so it lands on the same slot. */
    if (desc->event_type == ADV_RPT_SCAN_RSP) {
        uint8_t rn = desc->length_data;
        if (rn > SCAN_RAW_MAX) rn = SCAN_RAW_MAX;
        memcpy(slot->rsp, desc->data, rn);
        slot->rsp_len = rn;
        parse_mfg(desc->data, rn, slot, 1);
        xSemaphoreGive(s_mtx);
        return;
    }

    /* Estimate the advertising interval with an EMA over gaps (20 ms .. 10 s only) */
    if (slot->count > 0) {
        uint32_t gap = t - slot->last_ms;
        if (gap >= 20 && gap <= 10000) {
            slot->itvl_ms = slot->itvl_ms ? (uint16_t)((slot->itvl_ms * 3u + gap) / 4u)
                                          : (uint16_t)gap;
        }
    }

    slot->count++;
    slot->last_ms = t;
    slot->adv_type = desc->event_type;   /* a slot may have been created by a scan response */
    slot->rssi_last = desc->rssi;
    if (desc->rssi > slot->rssi_max) slot->rssi_max = desc->rssi;
    slot->rssi_avg = (int8_t)((slot->rssi_avg * 7 + desc->rssi) / 8);   /* IIR smoothing */

    /* Raw bytes: replayed verbatim, independent of any parsing above */
    uint8_t n = desc->length_data;
    if (n > SCAN_RAW_MAX) n = SCAN_RAW_MAX;
    memcpy(slot->raw, desc->data, n);
    slot->raw_len = n;

    parse_adv(desc->data, desc->length_data, slot);
    parse_mfg(desc->data, desc->length_data, slot, 0);

    /* Throttle logging to one line per device per 5 minutes */
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
        aggregate(&ev->disc);          /* parse + insert only, never do I/O here */
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

    uint32_t itvl = sc->itvl_ms < 20 ? 20 : sc->itvl_ms;
    uint32_t win  = sc->win_ms  < 2  ? 2  : sc->win_ms;
    if (win > itvl) win = itvl;

    /* Capture and advertising are exclusive, so the duty cap can be 50% (higher starves the SoftAP) */
    uint32_t cap = itvl / 2;
    if (cap < 2) cap = 2;
    if (win > cap) {
        ESP_LOGW(TAG, "scan duty %.0f%% too high -> capped 50%% (win=%ums)",
                 (double)win * 100.0 / (double)itvl, (unsigned)cap);
        win = cap;
    }
    s_itvl_ms = (uint16_t)itvl;
    s_win_ms  = (uint16_t)win;
    s_filter_noise = sc->filter_noise;
    s_filtered = 0;

    uint32_t u_itvl = itvl * 8 / 5;                 /* ms -> 0.625 ms units */
    uint32_t u_win  = win * 8 / 5;
    if (u_itvl < 4) u_itvl = 4;
    if (u_itvl > 0x4000) u_itvl = 0x4000;
    if (u_win < 4) u_win = 4;
    if (u_win > u_itvl) u_win = u_itvl;

    struct ble_gap_disc_params p = {0};
    p.itvl              = (uint16_t)u_itvl;
    p.window            = (uint16_t)u_win;
    p.passive           = 0;        /* active scan: needed to receive SCAN_RSP payloads */
    p.filter_duplicates = 0;        /* we aggregate ourselves; controller de-dup eats counts */

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, disc_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        applog_add(LOG_T_ERR, 0, "disc start rc=%d", rc);
        return ESP_FAIL;
    }
    s_running = true;
    s_since_ms = now_ms();
    s_pkts = 0;
    ESP_LOGI(TAG, "scan on: win=%ums itvl=%ums (%.0f%%) active",
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

    /* Newest first (insertion sort, n <= 32) */
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
