#include "web_server.h"
#include "app_cfg.h"
#include "app_log.h"
#include "beacon_adv.h"
#include "radio.h"
#include "beacon_scan.h"
#include "status_led.h"
#include "storage.h"
#include "clone_op.h"
#include "board.h"
#include "net_ap.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "cJSON.h"

static const char *TAG = "web";
static httpd_handle_t s_srv;

/* The SPA (main/www/index.html) is gzipped at build time and embedded:
 * 47.6 KB of HTML (UI strings for EN + zh) becomes ~15 KB of rodata. */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

/* ------------------------- helpers ------------------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *js = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!js) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, js);
    cJSON_free(js);
    return e;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "err", msg ? msg : "error");
    httpd_resp_set_status(req, status);
    return send_json(req, o);
}

static esp_err_t ok_msg(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    if (msg) cJSON_AddStringToObject(o, "msg", msg);
    return send_json(req, o);
}

/* Bearer-token auth. The AP password is the only key; the token is generated
 * randomly on first boot and stored in NVS. */
static bool auth_ok(httpd_req_t *req)
{
    char v[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", v, sizeof(v)) == ESP_OK) {
        const char *p = v;
        if (strncmp(p, "Bearer ", 7) == 0) p += 7;
        if (strcmp(p, cfg_api_token()) == 0) return true;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Token", v, sizeof(v)) == ESP_OK) {
        if (strcmp(v, cfg_api_token()) == 0) return true;
    }
    return false;
}

static char *read_body(httpd_req_t *req, size_t *out_len)
{
    int total = req->content_len;
    if (total <= 0 || total > 8192) return NULL;
    char *buf = malloc((size_t)total + 1);
    if (!buf) return NULL;
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, (size_t)(total - off));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); return NULL; }
        off += r;
    }
    buf[total] = '\0';
    if (out_len) *out_len = (size_t)total;
    return buf;
}

static int uri_last_id(const char *uri)
{
    const char *p = strrchr(uri, '/');
    if (!p || !p[1]) return -1;
    char *end = NULL;
    long v = strtol(p + 1, &end, 10);
    if (end && *end) return -1;
    return (int)v;
}

static double est_distance(int8_t ref_rssi, int8_t rssi)
{
    /* Log-distance path-loss model, n = 2.5 (typical indoors). Ordering hint only. */
    const double n = 2.5;
    double d = pow(10.0, ((double)ref_rssi - (double)rssi) / (10.0 * n));
    if (d < 0.1) d = 0.1;
    if (d > 100.0) d = 100.0;
    return d;
}

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
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

/* LED state that should be showing (kept in sync with main.c:update_led) */
static const char *led_state_name(rmode_t m, uint8_t sta)
{
    if (m == RMODE_CLONE) return sta > 0 ? "clone+client" : "clone";
    if (m == RMODE_SNIFF) return "sniff";
    return "idle";
}

/* ------------------------- static page ------------------------- */

/* ETag from the build (main/CMakeLists.txt): stable for a given firmware, so a revalidating
 * browser answers 304 instead of downloading the page again. */
#ifndef APP_UI_ETAG
#define APP_UI_ETAG "dev"
#endif

/* Two sources, fixed order: the uploaded file when it is usable, otherwise the blob that
 * lives in the firmware. The embedded copy is never removed, so a truncated upload, a failed
 * mount or `records format` can only cost the override -- never the ability to open the page. */
static esp_err_t h_index(httpd_req_t *req)
{
    static const char build_etag[] = "\"" APP_UI_ETAG "\"";
    char etag[24];
    char inm[48];
    const uint8_t *blob = index_html_gz_start;
    size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    uint32_t crc = 0;
    /* The read session opens before the ETag is chosen: stat() answering OK does not mean the
     * file is still readable (an upload may replace it between the two calls), and the page
     * must be labelled with the source that actually sends the body -- else a later 304 would
     * revalidate the embedded copy as if it were the uploaded one. */
    bool from_fs = storage_ui_stat(&crc, NULL) == ESP_OK && storage_ui_read_begin() == ESP_OK;

    if (from_fs) {
        snprintf(etag, sizeof(etag), "\"ui-%08x\"", (unsigned)crc);
    } else {
        snprintf(etag, sizeof(etag), "%s", build_etag);
    }

    httpd_resp_set_hdr(req, "ETag", etag);
    /* Always revalidate: a browser that cached the embedded page for a day would never see an
     * upload without a manual cache flush. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    bool fresh = httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
                 strcmp(inm, etag) == 0;
    if (fresh) {
        if (from_fs) storage_ui_read_end();   /* begin() is exclusive: only the holder releases */
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    if (!from_fs) return httpd_resp_send(req, (const char *)blob, (ssize_t)len);

    /* stream the file in 4 KB chunks: no whole-page allocation while the AP is serving.
     * The buffer is static because storage_ui_read_begin() is exclusive, so only one request
     * at a time reaches this loop (4 KB on the 7 KB httpd stack would be the alternative). */
    static uint8_t buf[4096];
    int n;
    esp_err_t err = ESP_OK;
    while ((n = storage_ui_read_chunk(buf, sizeof(buf))) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) { err = ESP_FAIL; break; }
    }
    if (n < 0) err = ESP_FAIL;
    storage_ui_read_end();
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);   /* terminate the chunked body */
}

/* ------------------------- session ------------------------- */

static esp_err_t h_session(httpd_req_t *req)
{
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(j, "password");
    if (!cJSON_IsString(pw) || !pw->valuestring) { cJSON_Delete(j); return fail(req, "400 Bad Request", "password required"); }

    cfg_lock();
    bool ok = strcmp(pw->valuestring, cfg_get()->ap.pass) == 0 || strcmp(pw->valuestring, cfg_api_token()) == 0;
    cfg_unlock();
    cJSON_Delete(j);
    if (!ok) return fail(req, "401 Unauthorized", "wrong password");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "token", cfg_api_token());
    return send_json(req, o);
}

/* ------------------------- status ------------------------- */

static esp_err_t h_status(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    cJSON *o = cJSON_CreateObject();
    cfg_lock();
    const beacon_cfg_t *c = cfg_get();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "ver", APP_VER);
    cJSON_AddNumberToObject(o, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "active", c->active);
    cJSON_AddNumberToObject(o, "count", c->count);
    cJSON_AddBoolToObject(o, "rotation", c->rotation);
    cJSON_AddNumberToObject(o, "rotation_s", c->rotation_s);
    cJSON_AddNumberToObject(o, "scan_itvl_ms", c->scan.itvl_ms);
    cJSON_AddNumberToObject(o, "scan_win_ms", c->scan.win_ms);
    cJSON_AddBoolToObject(o, "scan_filter", c->scan.filter_noise);
    cJSON_AddBoolToObject(o, "scan_full_duty", c->scan.full_duty);
    cJSON_AddBoolToObject(o, "scan_clear_rounds", c->scan.clear_each_round);
    cJSON_AddNumberToObject(o, "scan_duration_s", c->scan.duration_s);
    cfg_unlock();

    /* ---- serial-free diagnostics: enough to triage from a phone browser ---- */
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON_AddStringToObject(o, "reset_reason", reset_reason_str(rr));
    cJSON_AddNumberToObject(o, "boots", cfg_stat_get("boots"));
    cJSON_AddNumberToObject(o, "brownouts", cfg_stat_get("brownout"));
    {
        uint32_t starts = 0; int8_t tx = 0;
        beacon_adv_stats(&starts, &tx);
        cJSON_AddNumberToObject(o, "adv_starts", starts);
        cJSON_AddNumberToObject(o, "tx_dbm", tx);
    }
    {
        uint32_t pkts = 0, rms = 0;
        blescan_stats(&pkts, &rms);
        cJSON_AddNumberToObject(o, "scan_pkts", pkts);
        cJSON_AddNumberToObject(o, "scan_running_ms", rms);
        cJSON_AddNumberToObject(o, "scan_filtered", blescan_filtered());
    }
    cJSON_AddStringToObject(o, "led", led_state_name(radio_mode(), net_ap_sta_count()));
    cJSON_AddStringToObject(o, "board", BOARD_NAME);
    {
        char sb[12];
        cJSON_AddStringToObject(o, "storage", storage_stat_str(sb, sizeof(sb)));
    }

    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    cJSON_AddBoolToObject(o, "adv", beacon_adv_is_running());
    cJSON_AddBoolToObject(o, "scan", blescan_running());
    cJSON_AddNumberToObject(o, "detected", blescan_count());
    cJSON_AddBoolToObject(o, "ap", net_ap_is_up());
    cJSON_AddNumberToObject(o, "sta", net_ap_sta_count());
    cJSON_AddNumberToObject(o, "log_seq", applog_last_seq());

    char ip[16] = "192.168.4.1";
    esp_netif_t *nif = net_ap_netif();
    if (nif) {
        esp_netif_ip_info_t ipi;
        if (esp_netif_get_ip_info(nif, &ipi) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
        }
    }
    cJSON_AddStringToObject(o, "ip", ip);
    return send_json(req, o);
}

/* ------------------------- uploaded SPA ------------------------- */

/* An error response leaves the request body unread; on a keep-alive socket that would desync
 * the next request, so swallow it in 512 B steps. */
static void ui_drain(httpd_req_t *req, size_t want)
{
    uint8_t junk[512];
    while (want > 0) {
        int r = httpd_req_recv(req, (char *)junk, want > sizeof(junk) ? sizeof(junk) : want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT || r <= 0) break;
        want -= (size_t)r;
    }
}

/* The upload is dead either way (the caller aborts it): drop what is left of the body, then
 * say why -- a size verdict is the client's fault, anything else is the device's. */
static esp_err_t ui_chunk_fail(httpd_req_t *req, esp_err_t e, size_t left)
{
    ui_drain(req, left);
    if (e == ESP_ERR_INVALID_SIZE) return fail(req, "413 Payload Too Large", "body larger than 64 KB");
    return fail(req, "500 Internal Server Error", "write failed");
}

/* GET /api/ui -- which copy would / serve, and under which ETag */
static esp_err_t h_ui_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    uint32_t crc = 0;
    size_t len = 0;
    bool fs = storage_ui_stat(&crc, &len) == ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "source", fs ? "fs" : "embedded");
    if (fs) {
        cJSON_AddNumberToObject(o, "size", (double)len);
        char e[24];
        snprintf(e, sizeof(e), "ui-%08x", (unsigned)crc);
        cJSON_AddStringToObject(o, "etag", e);
    }
    return send_json(req, o);
}

/* POST /api/ui -- replace the uploaded SPA with a gzipped index.html */
static esp_err_t h_ui_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");

    /* refusals happen before any of the body is written, so the live file stays intact */
    size_t total = req->content_len;
    if (total < UI_MIN_BYTES) {
        ui_drain(req, total);
        return fail(req, "400 Bad Request", "body smaller than 1 KB");
    }
    if (total > UI_MAX_BYTES) {
        ui_drain(req, total);
        return fail(req, "413 Payload Too Large", "body larger than 64 KB");
    }
    if (!storage_ready()) return fail(req, "503 Service Unavailable", "archive unavailable");

    if (storage_ui_upload_begin() != ESP_OK) {
        ui_drain(req, total);
        /* begin() never waits for the mutex, so a failure while storage is up means a peer
         * holds it: either its own upload or a slow / stream. Retryable, not a bad payload. */
        if (!storage_ready()) return fail(req, "503 Service Unavailable", "archive unavailable");
        return fail(req, "409 Conflict", "upload or stream in progress, retry");
    }

    /* 2 KB, not the 4 KB that h_index avoids putting on this stack either: the httpd task
     * runs on 7 KB and the write path has to fit next to the server's own frames */
    uint8_t buf[2048];
    size_t got = 0;
    /* gzip magic is the only check the device can do; read both bytes first so a short first
     * recv is not mistaken for a foreign body */
    while (got < 2) {
        int r = httpd_req_recv(req, (char *)buf + got, 2 - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { storage_ui_upload_abort(); return fail(req, "400 Bad Request", "short body"); }
        got += (size_t)r;
    }
    if (buf[0] != 0x1f || buf[1] != 0x8b) {
        storage_ui_upload_abort();
        ui_drain(req, total - got);
        return fail(req, "400 Bad Request", "not gzip");
    }
    esp_err_t ce = storage_ui_upload_chunk(buf, got);
    if (ce != ESP_OK) { storage_ui_upload_abort(); return ui_chunk_fail(req, ce, total - got); }

    while (got < total) {
        size_t want = total - got;
        if (want > sizeof(buf)) want = sizeof(buf);
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { storage_ui_upload_abort(); return fail(req, "400 Bad Request", "short body"); }
        ce = storage_ui_upload_chunk(buf, (size_t)r);
        if (ce != ESP_OK) { storage_ui_upload_abort(); return ui_chunk_fail(req, ce, total - got - (size_t)r); }
        got += (size_t)r;
    }

    /* the whole body is in by now, so a commit failure leaves nothing to drain */
    if (storage_ui_upload_commit() != ESP_OK) return fail(req, "500 Internal Server Error", "commit failed");

    uint32_t crc = 0;
    size_t len = 0;
    storage_ui_stat(&crc, &len);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "size", (double)len);
    char et[24];
    snprintf(et, sizeof(et), "ui-%08x", (unsigned)crc);
    cJSON_AddStringToObject(o, "etag", et);
    return send_json(req, o);
}

/* DELETE /api/ui -- drop the override, back to the embedded page */
static esp_err_t h_ui_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    esp_err_t e = storage_ui_remove();
    /* nothing uploaded is the same end state as a successful revert */
    if (e == ESP_OK || e == ESP_ERR_NOT_FOUND) return ok_msg(req, "reverted to the embedded UI");
    /* remove() never waits for the mutex: unlinking under an open stream would truncate the
     * page in flight, so report busy and let the client retry */
    if (!storage_ready()) return fail(req, "503 Service Unavailable", "archive unavailable");
    return fail(req, "409 Conflict", "a request is streaming /, retry");
}

/* ------------------------- profiles CRUD ------------------------- */

static esp_err_t h_profiles_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    cfg_lock();
    cJSON *root = cfg_to_json(cfg_get());
    cfg_unlock();
    if (!root) return fail(req, "500 Internal Server Error", "oom");
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t h_profiles_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");

    beacon_profile_t p;
    char err[64] = {0};
    if (!cfg_profile_from_json(j, &p, err, sizeof(err))) {
        cJSON_Delete(j);
        return fail(req, "400 Bad Request", err);
    }
    cJSON_Delete(j);

    int id = -1;
    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    int slot = cfg_free_slot(c);
    if (slot >= 0) {
        c->p[slot] = p;
        c->count++;
        if (c->active < 0) c->active = slot;
        id = slot;
        cfg_save();
        applog_add(LOG_T_CFG, 0, "profile %d created: %s", slot, p.name);
    }
    cfg_unlock();

    if (id < 0) return fail(req, "409 Conflict", "max profiles reached");
    if (radio_mode() == RMODE_CLONE) radio_clone();

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "id", id);
    return send_json(req, o);
}

static esp_err_t h_profiles_put(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    int id = uri_last_id(req->uri);
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");

    beacon_profile_t p;
    char err[64] = {0};
    bool parse_ok = cfg_profile_from_json(j, &p, err, sizeof(err));
    cJSON_Delete(j);
    if (!parse_ok) return fail(req, "400 Bad Request", err);

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    bool found = (id >= 0 && id < c->count);
    if (found) {
        c->p[id] = p;
        cfg_save();
        applog_add(LOG_T_CFG, 0, "profile %d updated: %s", id, p.name);
    }
    bool is_active = found && c->active == id;
    cfg_unlock();

    if (!found) return fail(req, "404 Not Found", "no such profile");
    if (is_active && radio_mode() == RMODE_CLONE) radio_clone();
    return ok_msg(req, "updated");
}

static esp_err_t h_profiles_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    int id = uri_last_id(req->uri);
    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    bool found = (id >= 0 && id < c->count);
    bool stop = false;
    if (found) {
        for (int i = id; i < c->count - 1; i++) c->p[i] = c->p[i + 1];
        c->count--;
        if (c->active == id) { c->active = -1; stop = true; }
        else if (c->active > id) c->active--;
        cfg_save();
        applog_add(LOG_T_CFG, 0, "profile %d deleted", id);
    }
    cfg_unlock();
    if (!found) return fail(req, "404 Not Found", "no such profile");
    if (stop) radio_stop();
    return ok_msg(req, "deleted");
}

/* ------------------------- activate / rotation ------------------------- */

static esp_err_t h_active(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");
    const cJSON *ij = cJSON_GetObjectItemCaseSensitive(j, "id");
    if (!cJSON_IsNumber(ij)) { cJSON_Delete(j); return fail(req, "400 Bad Request", "id required"); }
    int id = ij->valueint;
    cJSON_Delete(j);

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    if (id < -1 || id >= c->count) { cfg_unlock(); return fail(req, "400 Bad Request", "id out of range"); }
    if (id >= 0 && !c->p[id].enabled) { cfg_unlock(); return fail(req, "400 Bad Request", "profile disabled"); }
    c->active = id;
    if (id >= 0) cfg_touch(c, id);
    cfg_save();
    cfg_unlock();

    esp_err_t e = (id >= 0) ? radio_clone() : radio_stop();
    if (id >= 0) {
        cfg_lock();
        applog_add(LOG_T_ACT, 0, "activate %d '%s'", id, cfg_get()->p[id].name);
        cfg_unlock();
    } else {
        applog_add(LOG_T_ACT, 0, "broadcast off");
    }
    if (e != ESP_OK) return fail(req, "500 Internal Server Error", "apply failed");
    return ok_msg(req, "active updated");
}

static esp_err_t h_rotation(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(j, "enabled");
    const cJSON *se = cJSON_GetObjectItemCaseSensitive(j, "seconds");
    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    if (cJSON_IsBool(en)) c->rotation = cJSON_IsTrue(en);
    if (cJSON_IsNumber(se)) {
        int s = se->valueint;
        c->rotation_s = (uint16_t)(s < 5 ? 5 : s > 3600 ? 3600 : s);
    }
    cfg_save();
    cfg_unlock();
    cJSON_Delete(j);
    return ok_msg(req, "rotation updated");
}

/* ------------------------- scan ------------------------- */

static esp_err_t h_scan_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char q[64];
    int limit = 32;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) {
            int l = atoi(v);
            if (l > 0 && l <= SCAN_TAB_MAX) limit = l;
        }
    }

    detection_t *tab = calloc((size_t)SCAN_TAB_MAX, sizeof(detection_t));
    if (!tab) return fail(req, "500 Internal Server Error", "oom");
    int n = blescan_snapshot(tab, SCAN_TAB_MAX);
    if (n > limit) n = limit;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "running", blescan_running());
    cJSON_AddNumberToObject(root, "count", n);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < n; i++) {
        const detection_t *d = &tab[i];
        cJSON *o = cJSON_CreateObject();
        char a[24];
        snprintf(a, sizeof(a), "%02x%02x%02x%02x%02x%02x",
                 d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
        cJSON_AddStringToObject(o, "addr", a);
        cJSON_AddNumberToObject(o, "addr_type", d->addr_type);
        cJSON_AddBoolToObject(o, "ibeacon", d->is_ibeacon);
        if (d->is_ibeacon) {
            char u[33];
            cfg_uuid_to_str(d->uuid, u);
            cJSON_AddStringToObject(o, "uuid", u);
            cJSON_AddNumberToObject(o, "major", d->major);
            cJSON_AddNumberToObject(o, "minor", d->minor);
            cJSON_AddNumberToObject(o, "ref_rssi", d->ref_rssi);
            cJSON_AddNumberToObject(o, "dist_m", est_distance(d->ref_rssi, d->rssi_avg));
        }
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddNumberToObject(o, "rssi", d->rssi_last);
        cJSON_AddNumberToObject(o, "rssi_max", d->rssi_max);
        cJSON_AddNumberToObject(o, "rssi_avg", d->rssi_avg);
        cJSON_AddNumberToObject(o, "count", d->count);
        cJSON_AddNumberToObject(o, "age_ms", now - d->last_ms);
        cJSON_AddNumberToObject(o, "adv_len", d->raw_len);
        cJSON_AddNumberToObject(o, "adv_type", d->adv_type);
        cJSON_AddNumberToObject(o, "itvl_ms", d->itvl_ms);
        {
            char rh[SCAN_RAW_MAX * 2 + 1];
            cfg_hex_to_str(d->raw, d->raw_len, rh);
            cJSON_AddStringToObject(o, "raw", rh);
            cJSON_AddBoolToObject(o, "clonable", d->raw_len >= 2);
        }
        if (d->mfg_len || d->mfg_company) {
            char mh[sizeof(d->mfg) * 2 + 1];
            cfg_hex_to_str(d->mfg, d->mfg_len, mh);
            cJSON_AddNumberToObject(o, "mfg_company", d->mfg_company);
            cJSON_AddStringToObject(o, "mfg_vendor", blescan_company_name(d->mfg_company));
            cJSON_AddStringToObject(o, "mfg", mh);
            cJSON_AddNumberToObject(o, "mfg_len", d->mfg_len);
            cJSON_AddNumberToObject(o, "mfg_src", d->mfg_src);
        }
        if (d->svc_uuid) {
            char sh[8];
            snprintf(sh, sizeof(sh), "0x%04X", d->svc_uuid);
            cJSON_AddStringToObject(o, "svc", sh);
            cJSON_AddStringToObject(o, "svc_name", blescan_service_name(d->svc_uuid));
        }
        {
            const char *nv = blescan_name_vendor(d->name);
            if (nv) cJSON_AddStringToObject(o, "name_vendor", nv);
        }
        cJSON_AddNumberToObject(o, "rsp_len", d->rsp_len);
        if (d->rsp_len) {
            char sh[SCAN_RAW_MAX * 2 + 1];
            cfg_hex_to_str(d->rsp, d->rsp_len, sh);
            cJSON_AddStringToObject(o, "rsp", sh);
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(tab);
    return send_json(req, root);
}

static esp_err_t h_scan_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(j, "itvl_ms");
    const cJSON *wv = cJSON_GetObjectItemCaseSensitive(j, "win_ms");
    const cJSON *fv = cJSON_GetObjectItemCaseSensitive(j, "filter_noise");
    const cJSON *fdv = cJSON_GetObjectItemCaseSensitive(j, "full_duty");
    const cJSON *fcv = cJSON_GetObjectItemCaseSensitive(j, "clear_each_round");
    const cJSON *dv  = cJSON_GetObjectItemCaseSensitive(j, "duration_s");
    if (cJSON_IsNumber(iv))c->scan.itvl_ms = (uint16_t)(iv->valueint < 20 ? 20 : iv->valueint > 10240 ? 10240 : iv->valueint);
    if (cJSON_IsNumber(wv))c->scan.win_ms  = (uint16_t)(wv->valueint < 2 ? 2 : wv->valueint > 10240 ? 10240 : wv->valueint);
    if (cJSON_IsBool(fv))  c->scan.filter_noise = cJSON_IsTrue(fv);
    if (cJSON_IsBool(fdv)) c->scan.full_duty = cJSON_IsTrue(fdv);
    if (cJSON_IsBool(fcv)) c->scan.clear_each_round = cJSON_IsTrue(fcv);
    if (cJSON_IsNumber(dv))
        c->scan.duration_s = (uint16_t)(dv->valueint < 10 ? 10 : dv->valueint > 900 ? 900 : dv->valueint);
    cfg_save();
    cfg_unlock();
    cJSON_Delete(j);

    /* Scanning is manual-only, so saving the parameters just restarts an open sniff window
     * with the new interval/window (and restarts its 60 s). */
    if (radio_mode() == RMODE_SNIFF) {
        esp_err_t e = radio_sniff();
        if (e != ESP_OK) return fail(req, "500 Internal Server Error", "scan restart failed");
    }
    return ok_msg(req, "scan updated");
}

static esp_err_t h_scan_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    blescan_clear();
    return ok_msg(req, "cleared");
}

/* ------------------------- capture archive (persistent) ------------------------- */

static cJSON *cap_rec_json(const cap_rec_t *r)
{
    cJSON *o = cJSON_CreateObject();
    char h[CFG_RAW_MAX * 2 + 1], u[33];

    cfg_hex_to_str(r->addr, 6, h);
    cJSON_AddStringToObject(o, "addr", h);
    cJSON_AddNumberToObject(o, "addr_type", r->addr_type);
    cJSON_AddNumberToObject(o, "adv_type", r->adv_type);
    cJSON_AddStringToObject(o, "name", r->name);
    cJSON_AddNumberToObject(o, "src", r->src);

    cfg_hex_to_str(r->raw, r->raw_len, h);
    cJSON_AddStringToObject(o, "raw", h);
    cJSON_AddNumberToObject(o, "raw_len", r->raw_len);
    if (r->rsp_len) {
        cfg_hex_to_str(r->rsp, r->rsp_len, h);
        cJSON_AddStringToObject(o, "rsp", h);
    }
    cJSON_AddNumberToObject(o, "rsp_len", r->rsp_len);

    if (r->mfg_len || r->mfg_company) {
        char mh[sizeof(r->mfg) * 2 + 1];
        cfg_hex_to_str(r->mfg, r->mfg_len, mh);
        cJSON_AddNumberToObject(o, "mfg_company", r->mfg_company);
        cJSON_AddStringToObject(o, "mfg_vendor", blescan_company_name(r->mfg_company));
        cJSON_AddStringToObject(o, "mfg", mh);
        cJSON_AddNumberToObject(o, "mfg_len", r->mfg_len);
        cJSON_AddNumberToObject(o, "mfg_src", r->mfg_src);
    }
    if (r->svc_uuid) {
        char sh[8];
        snprintf(sh, sizeof(sh), "0x%04X", r->svc_uuid);
        cJSON_AddStringToObject(o, "svc", sh);
        cJSON_AddStringToObject(o, "svc_name", blescan_service_name(r->svc_uuid));
    }

    cJSON_AddBoolToObject(o, "ibeacon", r->is_ibeacon);
    if (r->is_ibeacon) {
        cfg_uuid_to_str(r->uuid, u);
        cJSON_AddStringToObject(o, "uuid", u);
        cJSON_AddNumberToObject(o, "major", r->major);
        cJSON_AddNumberToObject(o, "minor", r->minor);
        cJSON_AddNumberToObject(o, "ref_rssi", r->ref_rssi);
    }

    cJSON_AddNumberToObject(o, "rssi", r->rssi_last);
    cJSON_AddNumberToObject(o, "rssi_max", r->rssi_max);
    cJSON_AddNumberToObject(o, "rssi_avg", r->rssi_avg);
    cJSON_AddNumberToObject(o, "packets", r->count);
    cJSON_AddNumberToObject(o, "itvl_ms", r->itvl_ms);
    cJSON_AddNumberToObject(o, "dwell_ms", r->dwell_ms);
    cJSON_AddNumberToObject(o, "boot_seq", r->boot_seq);
    cJSON_AddNumberToObject(o, "uptime_s", r->uptime_s);
    return o;
}

static esp_err_t h_history_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");

    cap_rec_t *recs = calloc(CAP_SLOTS, sizeof(cap_rec_t));
    if (!recs) return fail(req, "500 Internal Server Error", "oom");
    int n = storage_capture_list(recs, CAP_SLOTS);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "available", storage_ready());
    cJSON_AddNumberToObject(root, "count", n);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cap_rec_json(&recs[i]));
    free(recs);
    return send_json(req, root);
}

/* ------------------------- favourites ------------------------- */

static esp_err_t h_pins_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");

    cap_rec_t *recs = calloc(PIN_SLOTS, sizeof(cap_rec_t));
    if (!recs) return fail(req, "500 Internal Server Error", "oom");
    int n = storage_pin_list(recs, PIN_SLOTS);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "available", storage_ready());
    cJSON_AddNumberToObject(root, "count", n);
    cJSON_AddNumberToObject(root, "slots", PIN_SLOTS);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < n; i++) {
        cJSON *o = cap_rec_json(&recs[i]);
        cJSON_AddNumberToObject(o, "id", i);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);
    return send_json(req, root);
}

/* Pin the n-th newest archive row: the archive is read newest-first, which is what the UI
 * shows, so the index the user clicked is the index used here. */
static esp_err_t h_pins_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");
    const cJSON *ij = cJSON_GetObjectItemCaseSensitive(j, "index");
    int idx = cJSON_IsNumber(ij) ? ij->valueint : -1;
    cJSON_Delete(j);
    if (idx < 0) return fail(req, "400 Bad Request", "index required");

    cap_rec_t *recs = calloc(CAP_SLOTS, sizeof(cap_rec_t));
    if (!recs) return fail(req, "500 Internal Server Error", "oom");
    int n = storage_capture_list(recs, CAP_SLOTS);
    if (idx >= n) {
        free(recs);
        return fail(req, "404 Not Found", "no such archive row (list changed?)");
    }
    esp_err_t e = storage_pin_add(&recs[idx]);
    char addr[16];
    cfg_hex_to_str(recs[idx].addr, 6, addr);
    free(recs);
    if (e == ESP_ERR_NO_MEM)  return fail(req, "409 Conflict", "favourites full");
    if (e != ESP_OK)          return fail(req, "500 Internal Server Error", "pin failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "addr", addr);
    applog_add(LOG_T_CFG, 0, "pin %s", addr);
    return send_json(req, o);
}

static esp_err_t h_pins_delete(httpd_req_t *req)      /* DELETE /api/pins -> clear all */
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    esp_err_t e = storage_pin_clear();
    if (e != ESP_OK) return fail(req, "500 Internal Server Error", "clear failed");
    return ok_msg(req, "favourites cleared");
}

static esp_err_t h_pin_item_delete(httpd_req_t *req)  /* DELETE /api/pins/<n> */
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    esp_err_t e = storage_pin_remove(uri_last_id(req->uri));
    if (e == ESP_ERR_INVALID_ARG) return fail(req, "404 Not Found", "no such favourite");
    if (e != ESP_OK)              return fail(req, "500 Internal Server Error", "unpin failed");
    return ok_msg(req, "unpinned");
}

static esp_err_t h_history_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    esp_err_t e = storage_capture_clear();
    if (e != ESP_OK) return fail(req, "500 Internal Server Error", "clear failed");
    return ok_msg(req, "history cleared");
}

/* ------------------------- one-tap clone ------------------------- */

static esp_err_t h_clone(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");

    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "addr");
    uint8_t addr[6];
    bool ok_addr = cJSON_IsString(a) && cfg_hex_from_str(a->valuestring, addr, 6);
    cJSON_Delete(j);
    if (!ok_addr) return fail(req, "400 Bad Request", "addr must be 12 hex chars");

    /* The clone itself lives in clone_op.c: the USB console does the same thing */
    char msg[64] = {0};
    int id = -1;
    uint16_t itvl = 0;
    bool replaced = false;
    esp_err_t e = clone_op_from_addr(addr, &id, &itvl, &replaced, msg, sizeof(msg));
    if (e == ESP_ERR_NOT_FOUND)     return fail(req, "404 Not Found", msg);
    if (e == ESP_ERR_INVALID_STATE) return fail(req, "409 Conflict", msg);
    if (e != ESP_OK)                return fail(req, "500 Internal Server Error", "clone apply failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    cJSON_AddNumberToObject(o, "adv_itvl_ms", itvl);
    cJSON_AddBoolToObject(o, "replaced", replaced);
    return send_json(req, o);
}

static esp_err_t h_mode(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mode");
    char mode[12] = {0};
    if (cJSON_IsString(m)) snprintf(mode, sizeof(mode), "%s", m->valuestring);
    cJSON_Delete(j);

    esp_err_t e;
    if      (!strcmp(mode, "sniff")) e = radio_sniff();
    else if (!strcmp(mode, "clone")) e = radio_clone();
    else if (!strcmp(mode, "idle"))  e = radio_stop();
    else return fail(req, "400 Bad Request", "mode must be sniff|clone|idle");

    if (e != ESP_OK) return fail(req, "500 Internal Server Error", "mode switch failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    return send_json(req, o);
}

/* ------------------------- log ------------------------- */

typedef struct { cJSON *arr; } log_ctx_t;

static void log_emit(const logrec_t *r, void *ctx)
{
    log_ctx_t *c = (log_ctx_t *)ctx;
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddNumberToObject(o, "seq", r->seq);
    cJSON_AddNumberToObject(o, "up_ms", r->up_ms);
    cJSON_AddStringToObject(o, "type", applog_type_str(r->type));
    cJSON_AddNumberToObject(o, "rssi", r->rssi);
    cJSON_AddStringToObject(o, "msg", r->msg);
    cJSON_AddItemToArray(c->arr, o);
}

static esp_err_t h_log_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char q[64];
    uint32_t since = 0;
    int limit = 100;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) since = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) {
            int l = atoi(v);
            if (l > 0 && l <= LOG_RING_SZ) limit = l;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    log_ctx_t ctx = { arr };
    int n = applog_snapshot(since, limit, log_emit, &ctx);
    cJSON_AddNumberToObject(root, "n", n);
    cJSON_AddNumberToObject(root, "last_seq", applog_last_seq());
    return send_json(req, root);
}

static esp_err_t h_log_delete(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    applog_clear();
    return ok_msg(req, "cleared");
}

/* ------------------------- import / export ------------------------- */

static esp_err_t h_export(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    cfg_lock();
    cJSON *root = cfg_to_json(cfg_get());
    cfg_unlock();
    if (!root) return fail(req, "500 Internal Server Error", "oom");
    char *js = cJSON_Print(root);
    cJSON_Delete(root);
    if (!js) return fail(req, "500 Internal Server Error", "oom");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"beacon-config.json\"");
    esp_err_t e = httpd_resp_sendstr(req, js);
    cJSON_free(js);
    return e;
}

static esp_err_t h_import(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    beacon_cfg_t tmp;
    char err[64] = {0};
    bool ok = cfg_from_json(body, &tmp, err, sizeof(err));
    free(body);
    if (!ok) return fail(req, "400 Bad Request", err);

    cfg_lock();
    *cfg_get() = tmp;
    cfg_save();
    cfg_unlock();
    applog_add(LOG_T_CFG, 0, "config imported (%d profiles)", tmp.count);
    if (radio_mode() == RMODE_CLONE) radio_clone();
    return ok_msg(req, "imported");
}

/* ------------------------- network / system ------------------------- */

static esp_err_t h_ap(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    char *body = read_body(req, NULL);
    if (!body) return fail(req, "400 Bad Request", "body required");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return fail(req, "400 Bad Request", "bad json");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(j, "pass");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid)) {
        e = net_ap_set_creds(ssid->valuestring,
                             cJSON_IsString(pass) ? pass->valuestring : NULL);
    }
    cJSON_Delete(j);
    if (e != ESP_OK) return fail(req, "400 Bad Request", "invalid ssid/pass");
    return ok_msg(req, "ap updated, reconnect with new credentials");
}

static void reboot_cb(void *arg) { esp_restart(); }

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    applog_add(LOG_T_BOOT, 0, "reboot requested");
    esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 600000);
    return ok_msg(req, "rebooting");
}

static esp_err_t h_factory(httpd_req_t *req)
{
    if (!auth_ok(req)) return fail(req, "401 Unauthorized", "token required");
    cfg_factory_reset();
    radio_stop();
    return ok_msg(req, "factory reset done, reboot to apply AP settings");
}

/* ------------------------- registration ------------------------- */

#define REG(u_, m_, h_) do {                                  \
        httpd_uri_t t = { .uri = (u_), .method = (m_),         \
                          .handler = (h_), .user_ctx = NULL }; \
        httpd_register_uri_handler(s_srv, &t);                 \
    } while (0)

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 36;
    cfg.stack_size       = 7168;      /* cJSON serialisation needs the depth */
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t e = httpd_start(&s_srv, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        return e;
    }

    REG("/",                   HTTP_GET, h_index);
    REG("/api/session",        HTTP_POST, h_session);
    REG("/api/status",         HTTP_GET, h_status);
    REG("/api/profiles",       HTTP_GET, h_profiles_get);
    REG("/api/profiles",       HTTP_POST, h_profiles_post);
    REG("/api/profiles/*",     HTTP_PUT, h_profiles_put);
    REG("/api/profiles/*",     HTTP_DELETE, h_profiles_delete);
    REG("/api/history",        HTTP_GET, h_history_get);
    REG("/api/history",        HTTP_DELETE, h_history_delete);
    REG("/api/pins",           HTTP_GET, h_pins_get);
    REG("/api/pins",           HTTP_POST, h_pins_post);
    REG("/api/pins",           HTTP_DELETE, h_pins_delete);
    REG("/api/pins/*",         HTTP_DELETE, h_pin_item_delete);
    REG("/api/clone",          HTTP_POST, h_clone);
    REG("/api/mode",           HTTP_POST, h_mode);
    REG("/api/active",         HTTP_POST, h_active);
    REG("/api/rotation",       HTTP_POST, h_rotation);
    REG("/api/scan",           HTTP_GET, h_scan_get);
    REG("/api/scan",           HTTP_POST, h_scan_post);
    REG("/api/scan",           HTTP_DELETE, h_scan_delete);
    REG("/api/log",            HTTP_GET, h_log_get);
    REG("/api/log",            HTTP_DELETE, h_log_delete);
    REG("/api/export",         HTTP_GET, h_export);
    REG("/api/import",         HTTP_POST, h_import);
    REG("/api/ap",             HTTP_POST, h_ap);
    REG("/api/reboot",         HTTP_POST, h_reboot);
    REG("/api/factory",        HTTP_POST, h_factory);
    REG("/api/ui",             HTTP_GET,    h_ui_get);
    REG("/api/ui",             HTTP_POST,   h_ui_post);
    REG("/api/ui",             HTTP_DELETE, h_ui_delete);

    ESP_LOGI(TAG, "http server on :80");
    return ESP_OK;
}

void web_stop(void)
{
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
}
