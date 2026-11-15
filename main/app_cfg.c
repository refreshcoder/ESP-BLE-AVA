#include "app_cfg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "cJSON.h"
#include "app_log.h"

static const char *TAG     = "cfg";
static const char *NVS_NS  = "beacon";
static const char *K_CFG   = "cfg";
static const char *K_TOKEN = "token";

static beacon_cfg_t     s_cfg;
static SemaphoreHandle_t s_mtx;
static char             s_token[33];

/* ---------------- hex helpers ---------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool cfg_uuid_from_str(const char *s, uint8_t out[CFG_UUID_LEN])
{
    if (!s) return false;
    uint8_t buf[CFG_UUID_LEN];
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-' || *p == ':' || *p == ' ') continue;
        int h = hexval(*p);
        if (h < 0) return false;
        if (n >= CFG_UUID_LEN * 2) return false;
        if (n & 1) buf[n / 2] = (uint8_t)((buf[n / 2] << 4) | h);
        else      buf[n / 2] = (uint8_t)h;
        n++;
    }
    if (n != CFG_UUID_LEN * 2) return false;
    memcpy(out, buf, CFG_UUID_LEN);
    return true;
}

void cfg_uuid_to_str(const uint8_t u[CFG_UUID_LEN], char out[33])
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < CFG_UUID_LEN; i++) {
        out[i * 2]     = H[u[i] >> 4];
        out[i * 2 + 1] = H[u[i] & 0x0F];
    }
    out[32] = '\0';
}

void cfg_hex_to_str(const uint8_t *in, int n, char *out)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i*2] = H[in[i]>>4]; out[i*2+1] = H[in[i]&0x0F]; }
    out[n*2] = '\0';
}

bool cfg_hex_from_str(const char *s, uint8_t *out, int n)
{
    if (!s || (int)strlen(s) != n * 2) return false;
    for (int i = 0; i < n; i++) {
        int h = hexval(s[i*2]), l = hexval(s[i*2+1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

/* ---------------- defaults ---------------- */

static int8_t clamp8(int v, int lo, int hi) { return (int8_t)(v < lo ? lo : v > hi ? hi : v); }

void cfg_defaults(beacon_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->ver        = 1;
    c->count      = 1;
    c->active     = 0;
    c->rotation   = false;
    c->rotation_s = 30;
    c->last_mode  = CFG_MODE_SNIFF;   /* first boot after a factory reset starts parked */
    c->scan.itvl_ms = 300;
    c->scan.win_ms  = 60;
    c->scan.itvl_ms = 300;
    c->scan.win_ms  = 60;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(c->ap.ssid, sizeof(c->ap.ssid), "BeaconGW-%02X%02X", mac[4], mac[5]);
    snprintf(c->ap.pass, sizeof(c->ap.pass), "beacon%02x%02x", mac[4], mac[5]);

    beacon_profile_t *p = &c->p[0];
    snprintf(p->name, sizeof(p->name), "default beacon");
    p->enabled      = true;
    /* Derive a private, v4-shaped UUID from the MAC so we never collide with a
     * real vendor identifier by accident. */
    uint8_t seed[6];
    memcpy(seed, mac, 6);
    for (int i = 0; i < CFG_UUID_LEN; i++) p->uuid[i] = (uint8_t)(seed[i % 6] * 31u + i * 97u + 0x5Au);
    p->uuid[6] = (uint8_t)((p->uuid[6] & 0x0F) | 0x40);
    p->uuid[8] = (uint8_t)((p->uuid[8] & 0x3F) | 0x80);
    p->major       = 1;
    p->minor       = 1;
    p->ref_rssi    = -59;
    p->radio_dbm   = 9;
    p->adv_itvl_ms = 100;
    p->scannable   = false;
    p->use_rand_addr = false;
}

/* ---------------- JSON <-> struct ---------------- */

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valueint : def;
}
static bool jbool(const cJSON *o, const char *k, bool def)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(i)) return cJSON_IsTrue(i);
    if (cJSON_IsNumber(i)) return i->valueint != 0;
    return def;
}
static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

/* Validate and fill one profile; writes a message into err on failure */
bool cfg_profile_from_json(const cJSON *j, beacon_profile_t *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof(*p));
    const char *s;

    s = jstr(j, "name");
    if (!s || !*s) { snprintf(err, errlen, "name required"); return false; }
    if (strlen(s) >= CFG_NAME_MAX) { snprintf(err, errlen, "name too long"); return false; }
    strncpy(p->name, s, CFG_NAME_MAX - 1);

    int kind = jint(j, "kind", PROFILE_KIND_IBEACON);
    if (kind != PROFILE_KIND_IBEACON && kind != PROFILE_KIND_RAW) {
        snprintf(err, errlen, "kind must be 0(ibeacon) or 1(raw)"); return false;
    }
    p->kind = (uint8_t)kind;

    if (p->kind == PROFILE_KIND_RAW) {
        s = jstr(j, "raw");
        if (!s || !*s) { snprintf(err, errlen, "raw required for raw mode"); return false; }
        int len = (int)(strlen(s) / 2);
        if (len < 2 || len > CFG_RAW_MAX) { snprintf(err, errlen, "raw must be 2..31 bytes hex"); return false; }
        if (!cfg_hex_from_str(s, p->raw, len)) { snprintf(err, errlen, "raw is not valid hex"); return false; }
        p->raw_len = (uint8_t)len;
        /* optional: the scan response captured alongside the advertisement */
        s = jstr(j, "rsp");
        if (s && *s) {
            int rl = (int)(strlen(s) / 2);
            if (rl < 1 || rl > CFG_RAW_MAX || !cfg_hex_from_str(s, p->rsp, rl)) {
                snprintf(err, errlen, "rsp must be 1..31 bytes hex"); return false;
            }
            p->rsp_len = (uint8_t)rl;
        }
        p->ref_rssi = -59;
        /* If the request also carries a uuid, parse it for display; optional. */
        cfg_uuid_from_str(jstr(j, "uuid"), p->uuid);
        p->major = (uint16_t)jint(j, "major", 0);
        p->minor = (uint16_t)jint(j, "minor", 0);
    } else {
        s = jstr(j, "uuid");
        if (!cfg_uuid_from_str(s, p->uuid)) { snprintf(err, errlen, "uuid must be 32 hex chars"); return false; }

        int major = jint(j, "major", 0), minor = jint(j, "minor", 0);
        if (major < 0 || major > 65535 || minor < 0 || minor > 65535) {
            snprintf(err, errlen, "major/minor out of range"); return false;
        }
        p->major = (uint16_t)major;
        p->minor = (uint16_t)minor;
    }

    p->ref_rssi  = clamp8(jint(j, "ref_rssi", -59), -100, 0);
    p->radio_dbm = clamp8(jint(j, "radio_dbm", 9), -12, 21);

    int itvl = jint(j, "adv_itvl_ms", 100);
    if (itvl < 20 || itvl > 10240) { snprintf(err, errlen, "adv_itvl_ms 20..10240"); return false; }
    p->adv_itvl_ms = (uint16_t)itvl;

    p->enabled    = jbool(j, "enabled", true);
    p->scannable  = jbool(j, "scannable", false);

    s = jstr(j, "rand_addr");
    if (s && *s) {
        if (!cfg_hex_from_str(s, p->rand_addr, 6)) { snprintf(err, errlen, "rand_addr must be 12 hex chars"); return false; }
        p->rand_addr[0] |= 0xC0;          /* display order: top two bits of the first octet */
        p->use_rand_addr = jbool(j, "use_rand_addr", true);
    } else {
        p->use_rand_addr = false;
    }
    p->clone_public_addr = jbool(j, "clone_public_addr", false);
    if (p->clone_public_addr) p->use_rand_addr = false;   /* the two cannot both apply */

    s = jstr(j, "src");
    bool src_ok = false;
    if (s && *s) src_ok = cfg_hex_from_str(s, p->src_addr, 6);
    if (p->clone_public_addr) {
        /* A forced public address must be present and unicast (bit 0 of the first octet = group). */
        if (!src_ok) { snprintf(err, errlen, "clone_public_addr needs src (12 hex chars)"); return false; }
        if (p->src_addr[0] & 0x01) { snprintf(err, errlen, "public address must be unicast"); return false; }
        p->use_rand_addr = false;
    }
    p->src_adv_type = (uint8_t)jint(j, "src_adv_type", 0);
    p->use_seq      = (uint32_t)jint(j, "use_seq", 0);
    return true;
}

void cfg_profile_to_json(const beacon_profile_t *p, cJSON *dst)
{
    char u[33];
    cfg_uuid_to_str(p->uuid, u);
    cJSON_AddStringToObject(dst, "name", p->name);
    cJSON_AddBoolToObject(dst, "enabled", p->enabled);
    cJSON_AddNumberToObject(dst, "kind", p->kind);
    if (p->kind == PROFILE_KIND_RAW) {
        char h[CFG_RAW_MAX * 2 + 1];
        cfg_hex_to_str(p->raw, p->raw_len, h);
        cJSON_AddStringToObject(dst, "raw", h);
        if (p->rsp_len) {
            cfg_hex_to_str(p->rsp, p->rsp_len, h);
            cJSON_AddStringToObject(dst, "rsp", h);
        }
    }
    cJSON_AddStringToObject(dst, "uuid", u);
    cJSON_AddNumberToObject(dst, "major", p->major);
    cJSON_AddNumberToObject(dst, "minor", p->minor);
    cJSON_AddNumberToObject(dst, "ref_rssi", p->ref_rssi);
    cJSON_AddNumberToObject(dst, "radio_dbm", p->radio_dbm);
    cJSON_AddNumberToObject(dst, "adv_itvl_ms", p->adv_itvl_ms);
    cJSON_AddBoolToObject(dst, "scannable", p->scannable);
    cJSON_AddBoolToObject(dst, "use_rand_addr", p->use_rand_addr);
    if (p->clone_public_addr) cJSON_AddBoolToObject(dst, "clone_public_addr", true);
    cJSON_AddNumberToObject(dst, "use_seq", p->use_seq);
    if (p->use_rand_addr) {
        char h[13];
        cfg_hex_to_str(p->rand_addr, 6, h);
        cJSON_AddStringToObject(dst, "rand_addr", h);
    }
    if (p->src_addr[0] | p->src_addr[1] | p->src_addr[2] |
        p->src_addr[3] | p->src_addr[4] | p->src_addr[5]) {
        char h[13];
        cfg_hex_to_str(p->src_addr, 6, h);
        cJSON_AddStringToObject(dst, "src", h);
        cJSON_AddNumberToObject(dst, "src_adv_type", p->src_adv_type);
    }
}

cJSON *cfg_to_json(const beacon_cfg_t *c)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "ver", c->ver);
    cJSON_AddNumberToObject(root, "count", c->count);
    cJSON_AddNumberToObject(root, "active", c->active);
    cJSON_AddNumberToObject(root, "seq_gen", c->seq_gen);

    cJSON *rot = cJSON_AddObjectToObject(root, "rotation");
    cJSON_AddBoolToObject(rot, "enabled", c->rotation);
    cJSON_AddNumberToObject(rot, "seconds", c->rotation_s);
    cJSON_AddNumberToObject(root, "mode", c->last_mode);

    cJSON *sc = cJSON_AddObjectToObject(root, "scan");
    cJSON_AddNumberToObject(sc, "itvl_ms", c->scan.itvl_ms);
    cJSON_AddNumberToObject(sc, "win_ms", c->scan.win_ms);

    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap, "ssid", c->ap.ssid);
    cJSON_AddStringToObject(ap, "pass", c->ap.pass);

    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    for (int i = 0; i < c->count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", i);
        cfg_profile_to_json(&c->p[i], o);
        cJSON_AddItemToArray(arr, o);
    }
    return root;
}

bool cfg_from_json(const char *js, beacon_cfg_t *c, char *err, size_t errlen)
{
    cJSON *root = cJSON_Parse(js);
    if (!root) { snprintf(err, errlen, "bad json"); return false; }

    beacon_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.ver = 1;

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(arr)) { snprintf(err, errlen, "profiles array required"); cJSON_Delete(root); return false; }

    int n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (n >= CFG_MAX_PROFILES) break;
        if (!cfg_profile_from_json(it, &tmp.p[n], err, errlen)) { cJSON_Delete(root); return false; }
        n++;
    }
    tmp.count   = n;
    tmp.seq_gen = (uint32_t)jint(root, "seq_gen", 0);
    tmp.active = jint(root, "active", n ? 0 : -1);
    if (tmp.active < -1 || tmp.active >= n) tmp.active = n ? 0 : -1;

    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    if (cJSON_IsObject(rot)) {
        tmp.rotation   = jbool(rot, "enabled", false);        int sec        = jint(rot, "seconds", 30);
        tmp.rotation_s = (uint16_t)(sec < 5 ? 5 : sec > 3600 ? 3600 : sec);
    int lm = jint(root, "mode", CFG_MODE_SNIFF);
    tmp.last_mode = (uint8_t)(lm < 0 || lm > CFG_MODE_IDLE ? CFG_MODE_SNIFF : lm);
    }
    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "scan");
    if (cJSON_IsObject(sc)) {
        int iv = jint(sc, "itvl_ms", 300), wv = jint(sc, "win_ms", 60);
        tmp.scan.itvl_ms = (uint16_t)(iv < 20 ? 20 : iv > 10240 ? 10240 : iv);
        tmp.scan.win_ms  = (uint16_t)(wv < 2 ? 2 : wv > 10240 ? 10240 : wv);
    } else {
        tmp.scan.itvl_ms = 300; tmp.scan.win_ms = 60;
    }

    /* AP credentials: derived from the MAC when absent. WPA2 needs >=8 chars. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(tmp.ap.ssid, sizeof(tmp.ap.ssid), "BeaconGW-%02X%02X", mac[4], mac[5]);
    snprintf(tmp.ap.pass, sizeof(tmp.ap.pass), "beacon%02x%02x", mac[4], mac[5]);
    const cJSON *apj = cJSON_GetObjectItemCaseSensitive(root, "ap");
    if (cJSON_IsObject(apj)) {
        const char *ssid = jstr(apj, "ssid");
        const char *pass = jstr(apj, "pass");
        if (ssid && strlen(ssid) >= 1 && strlen(ssid) <= CFG_SSID_MAX)
            strncpy(tmp.ap.ssid, ssid, CFG_SSID_MAX);
        if (pass && strlen(pass) >= 8 && strlen(pass) <= CFG_PASS_MAX)
            strncpy(tmp.ap.pass, pass, CFG_PASS_MAX);
    }

    cJSON_Delete(root);
    *c = tmp;
    return true;
}

/* ---------------- NVS ---------------- */

static void token_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_token);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, K_TOKEN, s_token, &len) != ESP_OK || strlen(s_token) != 32) {
        uint8_t r[16];
        esp_fill_random(r, sizeof(r));
        cfg_hex_to_str(r, 16, s_token);
        nvs_set_str(h, K_TOKEN, s_token);
        nvs_commit(h);
        ESP_LOGI(TAG, "generated api token");
    }
    nvs_close(h);
}

const char *cfg_api_token(void) { return s_token; }

esp_err_t cfg_save(void)
{
    cJSON *root = cfg_to_json(&s_cfg);
    if (!root) return ESP_ERR_NO_MEM;
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!js) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, K_CFG, js, strlen(js) + 1);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    cJSON_free(js);
    if (e != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    return e;
}

esp_err_t cfg_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    cfg_defaults(&s_cfg);
    token_load();

    nvs_handle_t h;
    size_t len = 0;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, K_CFG, NULL, &len);
    if (e == ESP_OK && len > 0 && len < 4096) {
        char *buf = malloc(len + 1);
        if (buf) {
            if (nvs_get_blob(h, K_CFG, buf, &len) == ESP_OK) {
                buf[len] = '\0';
                char err[64] = {0};
                beacon_cfg_t tmp;
                if (cfg_from_json(buf, &tmp, err, sizeof(err))) s_cfg = tmp;
                else ESP_LOGW(TAG, "stored cfg invalid (%s), using defaults", err);
            }
            free(buf);
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "cfg: %d profile(s), active=%d", s_cfg.count, s_cfg.active);
    return ESP_OK;
}

void cfg_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void cfg_unlock(void) { xSemaphoreGive(s_mtx); }
beacon_cfg_t *cfg_get(void) { return &s_cfg; }

int cfg_free_slot(const beacon_cfg_t *c) { return c->count < CFG_MAX_PROFILES ? c->count : -1; }

void cfg_touch(beacon_cfg_t *c, int idx)
{
    if (idx < 0 || idx >= c->count) return;
    c->p[idx].use_seq = ++c->seq_gen;
}

int cfg_pick_slot(beacon_cfg_t *c)
{
    int slot = cfg_free_slot(c);
    if (slot >= 0) return slot;

    /* Full: evict the profile with the smallest use_seq that is not active */
    int victim = -1;
    for (int i = 0; i < c->count; i++) {
        if (i == c->active) continue;
        if (victim < 0 || c->p[i].use_seq < c->p[victim].use_seq) victim = i;
    }
    return victim;   /* -1: only the active profile is left, refuse */
}

uint32_t cfg_stat_get(const char *key)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

void cfg_stat_bump(const char *key, uint32_t add)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t v = 0;
    nvs_get_u32(h, key, &v);
    v += add;
    if (nvs_set_u32(h, key, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

esp_err_t cfg_factory_reset(void)
{
    cfg_lock();
    cfg_defaults(&s_cfg);
    esp_err_t e = cfg_save();
    cfg_unlock();
    applog_add(LOG_T_CFG, 0, "factory reset");
    return e;
}

/* ---------------- BLE public address (applied once per boot in app_main) ---------------- */

static uint8_t s_boot_addr[6];
static bool    s_boot_addr_valid;

void cfg_note_boot_addr(const uint8_t mac[6])
{
    if (!mac) { s_boot_addr_valid = false; return; }
    memcpy(s_boot_addr, mac, 6);
    s_boot_addr_valid = true;
}

/* True when the active profile wants a public address other than the one this boot
 * advertises with; radio_clone() then schedules a reboot so it takes effect. */
bool cfg_addr_change_pending(void)
{
    cfg_lock();
    const beacon_cfg_t *c = cfg_get();
    int i = c->active;
    bool pending = (i >= 0 && i < c->count) && c->p[i].clone_public_addr &&
                   (!s_boot_addr_valid || memcmp(c->p[i].src_addr, s_boot_addr, 6) != 0);
    cfg_unlock();
    return pending;
}
