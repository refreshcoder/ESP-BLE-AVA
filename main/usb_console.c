#include "usb_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

/* IDF >= 5.4 moved these under driver/; 5.2/5.3 exposed them as esp_vfs_usb_serial_jtag.h.
 * The `driver` component re-exports both, so no extra REQUIRES entry is needed. */
#if __has_include("driver/usb_serial_jtag_vfs.h")
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/usb_serial_jtag.h"
#include "esp_vfs_usb_serial_jtag.h"
#endif

#include "app_cfg.h"
#include "app_log.h"
#include "beacon_adv.h"
#include "beacon_scan.h"
#include "board.h"
#include "clone_op.h"
#include "net_ap.h"
#include "radio.h"
#include "storage.h"

static const char *TAG = "usb";

#define LINE_MAX_LEN   96
#define TASK_STACK     5120

static const char *rst_str(esp_reset_reason_t r)
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

/* One JSON object per line, then the end marker the host waits for */
static void emit(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) {
        printf("#ERR oom\n#END\n");
        return;
    }
    printf("%s\n", s);
    free(s);
}

static void end_ok(void) { printf("#END\n"); }
static void end_err(const char *why) { printf("#ERR %s\n#END\n", why); }

/* ------------------------------- read side ------------------------------- */

static void cmd_help(void)
{
    printf("# status                        - one JSON line: mode, heap, counters, mode flags\n"
           "# scan                          - JSONL: the live capture table (raw/rsp/mfg hex)\n"
           "# profiles                      - JSONL: the stored profiles\n"
           "# records                       - JSONL: the persistent capture archive\n"
           "# records clear                 - empty the archive\n"
           "# records format                - format it (only while the archive is off)\n"
           "# logs [since_seq]              - JSONL: ring log records\n"
           "# mode sniff|clone|idle         - switch the radio (arbitrated by radio.c)\n"
           "# use <id>|-1                   - activate a profile / broadcast nothing\n"
           "# del <id>                       - delete a profile (stops advertising if it was on air)\n"
           "# clone <addr 12 hex>           - copy a row of the capture table into a profile\n"
           "# adv <hex 4..62>               - build/update the `usb-raw` profile from hex and broadcast\n"
           "# clear                         - empty the capture table\n"
           "# reboot                        - restart the board\n"
           "# factory yes                   - factory reset, then restart\n");
    end_ok();
}

static void cmd_status(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "ver", APP_VER);
    cJSON_AddStringToObject(o, "board", BOARD_NAME);
    cJSON_AddNumberToObject(o, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(o, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(o, "reset_reason", rst_str(esp_reset_reason()));
    cJSON_AddNumberToObject(o, "boots", cfg_stat_get("boots"));
    cJSON_AddNumberToObject(o, "brownouts", cfg_stat_get("brownout"));

    cfg_lock();
    const beacon_cfg_t *c = cfg_get();
    cJSON_AddNumberToObject(o, "count", c->count);
    cJSON_AddNumberToObject(o, "active", c->active);
    cJSON_AddBoolToObject(o, "rotation", c->rotation);
    cJSON_AddNumberToObject(o, "rotation_s", c->rotation_s);
    cJSON_AddNumberToObject(o, "scan_itvl_ms", c->scan.itvl_ms);
    cJSON_AddNumberToObject(o, "scan_win_ms", c->scan.win_ms);
    cJSON_AddBoolToObject(o, "scan_filter", c->scan.filter_noise);
    cJSON_AddBoolToObject(o, "scan_full_duty", c->scan.full_duty);
    cJSON_AddBoolToObject(o, "scan_clear_rounds", c->scan.clear_each_round);
    cJSON_AddNumberToObject(o, "scan_duration_s", c->scan.duration_s);
    cfg_unlock();

    uint32_t starts = 0;
    int8_t tx = 0;
    beacon_adv_stats(&starts, &tx);
    cJSON_AddNumberToObject(o, "adv_starts", starts);
    cJSON_AddNumberToObject(o, "tx_dbm", tx);

    uint32_t pkts = 0, rms = 0;
    blescan_stats(&pkts, &rms);
    cJSON_AddNumberToObject(o, "scan_pkts", pkts);
    cJSON_AddNumberToObject(o, "scan_running_ms", rms);
    cJSON_AddNumberToObject(o, "scan_filtered", blescan_filtered());

    char sb[12];
    cJSON_AddStringToObject(o, "storage", storage_stat_str(sb, sizeof(sb)));
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    cJSON_AddBoolToObject(o, "adv", beacon_adv_is_running());
    cJSON_AddBoolToObject(o, "scan", blescan_running());
    cJSON_AddNumberToObject(o, "detected", blescan_count());
    cJSON_AddBoolToObject(o, "ap", net_ap_is_up());
    cJSON_AddNumberToObject(o, "sta", net_ap_sta_count());
    cJSON_AddNumberToObject(o, "log_seq", applog_last_seq());
    emit(o);
    end_ok();
}

static void cmd_scan(void)
{
    detection_t *tab = calloc(SCAN_TAB_MAX, sizeof(detection_t));
    if (!tab) { end_err("oom"); return; }
    int n = blescan_snapshot(tab, SCAN_TAB_MAX);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    char h[SCAN_RAW_MAX * 2 + 1];

    for (int i = 0; i < n; i++) {
        const detection_t *d = &tab[i];
        cJSON *o = cJSON_CreateObject();
        cfg_hex_to_str(d->addr, 6, h);
        cJSON_AddStringToObject(o, "addr", h);
        cJSON_AddNumberToObject(o, "addr_type", d->addr_type);
        cJSON_AddNumberToObject(o, "adv_type", d->adv_type);
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddNumberToObject(o, "rssi", d->rssi_last);
        cJSON_AddNumberToObject(o, "rssi_max", d->rssi_max);
        cJSON_AddNumberToObject(o, "count", d->count);
        cJSON_AddNumberToObject(o, "age_ms", now - d->last_ms);
        cJSON_AddNumberToObject(o, "itvl_ms", d->itvl_ms);
        cJSON_AddNumberToObject(o, "raw_len", d->raw_len);
        cJSON_AddBoolToObject(o, "clonable", d->raw_len >= 2);
        cfg_hex_to_str(d->raw, d->raw_len, h);
        cJSON_AddStringToObject(o, "raw", h);
        if (d->rsp_len) {
            char sh[SCAN_RAW_MAX * 2 + 1];
            cfg_hex_to_str(d->rsp, d->rsp_len, sh);
            cJSON_AddStringToObject(o, "rsp", sh);
        }
        if (d->mfg_len || d->mfg_company) {
            char mh[sizeof(d->mfg) * 2 + 1];
            cfg_hex_to_str(d->mfg, d->mfg_len, mh);
            cJSON_AddNumberToObject(o, "mfg_company", d->mfg_company);
            cJSON_AddStringToObject(o, "mfg_vendor", blescan_company_name(d->mfg_company));
            cJSON_AddStringToObject(o, "mfg", mh);
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
        if (d->is_ibeacon) {
            char u[33];
            cfg_uuid_to_str(d->uuid, u);
            cJSON_AddBoolToObject(o, "ibeacon", true);
            cJSON_AddStringToObject(o, "uuid", u);
            cJSON_AddNumberToObject(o, "major", d->major);
            cJSON_AddNumberToObject(o, "minor", d->minor);
        }
        emit(o);
    }
    free(tab);
    end_ok();
}

static void cmd_profiles(void)
{
    beacon_profile_t snap[CFG_MAX_PROFILES];
    int count, active;
    cfg_lock();
    count  = cfg_get()->count;
    active = cfg_get()->active;
    if (count > CFG_MAX_PROFILES) count = CFG_MAX_PROFILES;
    memcpy(snap, cfg_get()->p, sizeof(snap[0]) * count);
    cfg_unlock();

    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cfg_profile_to_json(&snap[i], o);
        cJSON_AddNumberToObject(o, "id", i);
        cJSON_AddBoolToObject(o, "active", active == i);
        emit(o);
    }
    end_ok();
}

/* One archive row as a JSON line: also used for favourites, they are the same struct. */
static void emit_cap_rec(const cap_rec_t *r, int id)
{
    char h[CFG_RAW_MAX * 2 + 1];
    cJSON *o = cJSON_CreateObject();
    if (id >= 0) cJSON_AddNumberToObject(o, "id", id);
    cfg_hex_to_str(r->addr, 6, h);
    cJSON_AddStringToObject(o, "addr", h);
    cJSON_AddNumberToObject(o, "addr_type", r->addr_type);
    cJSON_AddNumberToObject(o, "adv_type", r->adv_type);
    cJSON_AddStringToObject(o, "name", r->name);
    cJSON_AddNumberToObject(o, "src", r->src);
    cfg_hex_to_str(r->raw, r->raw_len, h);
    cJSON_AddStringToObject(o, "raw", h);
    cJSON_AddNumberToObject(o, "raw_len", r->raw_len);
    cJSON_AddNumberToObject(o, "rsp_len", r->rsp_len);
    if (r->rsp_len) {
        cfg_hex_to_str(r->rsp, r->rsp_len, h);
        cJSON_AddStringToObject(o, "rsp", h);
    }
    if (r->mfg_len || r->mfg_company) {
        char mh[sizeof(r->mfg) * 2 + 1];
        cfg_hex_to_str(r->mfg, r->mfg_len, mh);
        cJSON_AddNumberToObject(o, "mfg_company", r->mfg_company);
        cJSON_AddStringToObject(o, "mfg_vendor", blescan_company_name(r->mfg_company));
        cJSON_AddStringToObject(o, "mfg", mh);
        cJSON_AddNumberToObject(o, "mfg_src", r->mfg_src);
    }
    if (r->svc_uuid) {
        char sh[8];
        snprintf(sh, sizeof(sh), "0x%04X", r->svc_uuid);
        cJSON_AddStringToObject(o, "svc", sh);
        cJSON_AddStringToObject(o, "svc_name", blescan_service_name(r->svc_uuid));
    }
    cJSON_AddBoolToObject(o, "ibeacon", r->is_ibeacon);
    cJSON_AddNumberToObject(o, "rssi", r->rssi_last);
    cJSON_AddNumberToObject(o, "rssi_max", r->rssi_max);
    cJSON_AddNumberToObject(o, "rssi_avg", r->rssi_avg);
    cJSON_AddNumberToObject(o, "packets", r->count);
    cJSON_AddNumberToObject(o, "itvl_ms", r->itvl_ms);
    cJSON_AddNumberToObject(o, "dwell_ms", r->dwell_ms);
    cJSON_AddNumberToObject(o, "boot_seq", r->boot_seq);
    cJSON_AddNumberToObject(o, "uptime_s", r->uptime_s);
    emit(o);
}

static void cmd_records(const char *arg)
{
    if (arg && !strcmp(arg, "format")) {
        esp_err_t e = storage_format();
        if (e != ESP_OK) { end_err(esp_err_to_name(e)); return; }
        end_ok();
        return;
    }
    if (arg && !strcmp(arg, "clear")) {
        esp_err_t e = storage_capture_clear();
        if (e != ESP_OK) { end_err("archive unavailable"); return; }
        end_ok();
        return;
    }

    if (!storage_ready()) { end_err("archive unavailable"); return; }
    cap_rec_t *recs = calloc(CAP_SLOTS, sizeof(cap_rec_t));
    if (!recs) { end_err("oom"); return; }
    int n = storage_capture_list(recs, CAP_SLOTS);
    for (int i = 0; i < n; i++) emit_cap_rec(&recs[i], -1);
    free(recs);
    end_ok();
}

/* ------------------------------ favourites ------------------------------ */

static void cmd_pins(const char *arg)
{
    if (!storage_ready()) { end_err("archive unavailable"); return; }
    if (arg && !strcmp(arg, "clear")) {
        esp_err_t e = storage_pin_clear();
        if (e != ESP_OK) { end_err(esp_err_to_name(e)); return; }
        end_ok();
        return;
    }
    cap_rec_t *recs = calloc(PIN_SLOTS, sizeof(cap_rec_t));
    if (!recs) { end_err("oom"); return; }
    int n = storage_pin_list(recs, PIN_SLOTS);
    for (int i = 0; i < n; i++) emit_cap_rec(&recs[i], i);
    free(recs);
    end_ok();
}

/* pin <n>: n is the index in `records` output (newest first), like the web UI's row order */
static void cmd_pin(const char *arg)
{
    if (!arg) { end_err("pin <n>  (index from 'records')"); return; }
    char *endp = NULL;
    long idx = strtol(arg, &endp, 10);
    if (endp == arg || idx < 0) { end_err("pin <n>"); return; }
    if (!storage_ready()) { end_err("archive unavailable"); return; }

    cap_rec_t *recs = calloc(CAP_SLOTS, sizeof(cap_rec_t));
    if (!recs) { end_err("oom"); return; }
    int n = storage_capture_list(recs, CAP_SLOTS);
    if (idx >= n) { free(recs); end_err("no such archive row"); return; }
    esp_err_t e = storage_pin_add(&recs[idx]);
    char addr[16];
    cfg_hex_to_str(recs[idx].addr, 6, addr);
    free(recs);
    if (e == ESP_ERR_NO_MEM) { end_err("favourites full"); return; }
    if (e != ESP_OK) { end_err(esp_err_to_name(e)); return; }
    applog_add(LOG_T_CFG, 0, "usb: pin %s", addr);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "addr", addr);
    emit(o);
    end_ok();
}

static void cmd_unpin(const char *arg)
{
    if (!arg) { end_err("unpin <n>  (index from 'pins')"); return; }
    char *endp = NULL;
    long idx = strtol(arg, &endp, 10);
    if (endp == arg || idx < 0) { end_err("unpin <n>"); return; }
    esp_err_t e = storage_pin_remove((int)idx);
    if (e != ESP_OK) { end_err(esp_err_to_name(e)); return; }
    end_ok();
}

static void log_emit(const logrec_t *r, void *ctx)
{
    (void)ctx;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq", r->seq);
    cJSON_AddNumberToObject(o, "up_ms", r->up_ms);
    cJSON_AddStringToObject(o, "type", applog_type_str(r->type));
    cJSON_AddNumberToObject(o, "rssi", r->rssi);
    cJSON_AddStringToObject(o, "msg", r->msg);
    emit(o);
}

static void cmd_logs(const char *since)
{
    uint32_t from = since ? (uint32_t)strtoul(since, NULL, 10) : 0;
    applog_snapshot(from, 32, log_emit, NULL);
    end_ok();
}

/* ------------------------------- write side ------------------------------ */

/* timing <itvl_ms> <win_ms> <duration_s>: scan timing without a browser */
static void cmd_scan_cfg(const char *a1, const char *a2, const char *a3)
{
    if (!a1 || !a2 || !a3) { end_err("timing <itvl_ms> <win_ms> <duration_s>"); return; }
    char *e1 = NULL, *e2 = NULL, *e3 = NULL;
    long itvl = strtol(a1, &e1, 10), win = strtol(a2, &e2, 10), dur = strtol(a3, &e3, 10);
    if (e1 == a1 || e2 == a2 || e3 == a3) { end_err("timing <itvl_ms> <win_ms> <duration_s>"); return; }
    cfg_lock();
    cfg_get()->scan.itvl_ms = (uint16_t)(itvl < 20 ? 20 : itvl > 10240 ? 10240 : itvl);
    cfg_get()->scan.win_ms  = (uint16_t)(win < 2 ? 2 : win > 10240 ? 10240 : win);
    cfg_get()->scan.duration_s = (uint16_t)(dur < 10 ? 10 : dur > 900 ? 900 : dur);
    cfg_save();
    uint16_t i2 = cfg_get()->scan.itvl_ms, w2 = cfg_get()->scan.win_ms, d2 = cfg_get()->scan.duration_s;
    cfg_unlock();

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "itvl_ms", i2);
    cJSON_AddNumberToObject(o, "win_ms", w2);
    cJSON_AddNumberToObject(o, "duration_s", d2);
    emit(o);
    end_ok();
}

/* filter on|off: drop personal-device noise before it takes a table slot */
static void cmd_filter(const char *arg)
{
    if (!arg || (strcmp(arg, "on") && strcmp(arg, "off"))) { end_err("filter on|off"); return; }
    cfg_lock();
    cfg_get()->scan.filter_noise = !strcmp(arg, "on");
    cfg_save();
    bool on = cfg_get()->scan.filter_noise;
    cfg_unlock();
    applog_add(LOG_T_SCAN, 0, "scan filter %s", on ? "on" : "off");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "filter_noise", on);
    emit(o);
    end_ok();
}

static void cmd_mode(const char *m)
{
    esp_err_t e;
    if      (!strcmp(m, "sniff")) e = radio_sniff();
    else if (!strcmp(m, "clone")) e = radio_clone();
    else if (!strcmp(m, "idle"))  e = radio_stop();
    else { end_err("mode must be sniff|clone|idle"); return; }
    if (e != ESP_OK) { end_err(esp_err_to_name(e)); return; }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    emit(o);
    end_ok();
}

static void cmd_use(const char *arg)
{
    if (!arg) { end_err("use <id>|-1"); return; }
    char *endp = NULL;
    long id = strtol(arg, &endp, 10);
    if (endp == arg) { end_err("use <id>|-1"); return; }

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    if (id < -1 || id >= c->count) { cfg_unlock(); end_err("id out of range"); return; }
    if (id >= 0 && !c->p[id].enabled) { cfg_unlock(); end_err("profile disabled"); return; }
    c->active = (int)id;
    if (id >= 0) cfg_touch(c, (int)id);
    cfg_save();
    cfg_unlock();

    esp_err_t e = (id >= 0) ? radio_clone() : radio_stop();
    if (e != ESP_OK) { end_err(esp_err_to_name(e)); return; }
    applog_add(LOG_T_ACT, 0, "usb: activate %ld", id);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "active", id);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    emit(o);
    end_ok();
}

static void cmd_del(const char *arg)
{
    if (!arg) { end_err("del <id>"); return; }
    char *endp = NULL;
    long id = strtol(arg, &endp, 10);
    if (endp == arg) { end_err("del <id>"); return; }

    bool stopped = false;
    if (profile_delete((int)id, &stopped) != ESP_OK) { end_err("no such profile"); return; }
    if (stopped) radio_stop();

    cfg_lock();
    int count = cfg_get()->count;
    cfg_unlock();

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "id", (int)id);
    cJSON_AddNumberToObject(o, "count", count);
    cJSON_AddBoolToObject(o, "stopped", stopped);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    emit(o);
    end_ok();
}

static void cmd_clone(const char *arg)
{
    uint8_t addr[6];
    if (!arg || !cfg_hex_from_str(arg, addr, 6)) { end_err("clone <addr 12 hex>"); return; }

    char msg[64] = {0};
    int id = -1;
    uint16_t itvl = 0;
    bool replaced = false;
    esp_err_t e = clone_op_from_addr(addr, &id, &itvl, &replaced, msg, sizeof(msg));
    if (e != ESP_OK) { end_err(msg[0] ? msg : esp_err_to_name(e)); return; }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddNumberToObject(o, "adv_itvl_ms", itvl);
    cJSON_AddBoolToObject(o, "replaced", replaced);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    emit(o);
    end_ok();
}

/* adv <hex>: the payload does not have to be on the air right now -- the console is also
 * how a profile gets restored on a board that is nowhere near the source. */
static void cmd_adv(const char *arg)
{
    size_t n = arg ? strlen(arg) : 0;
    if (n < 4 || n > (size_t)CFG_RAW_MAX * 2 || (n & 1)) { end_err("adv <hex 4..62 chars>"); return; }

    uint8_t raw[CFG_RAW_MAX];
    if (!cfg_hex_from_str(arg, raw, (int)(n / 2))) { end_err("adv <hex 4..62 chars>"); return; }

    char msg[64] = {0};
    int id = -1;
    uint16_t itvl = 0;
    bool replaced = false;
    esp_err_t e = clone_op_from_raw(raw, (uint8_t)(n / 2), NULL, 0, &id, &itvl, &replaced, msg, sizeof(msg));
    if (e != ESP_OK) { end_err(msg[0] ? msg : esp_err_to_name(e)); return; }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddNumberToObject(o, "raw_len", (int)(n / 2));
    cJSON_AddNumberToObject(o, "adv_itvl_ms", itvl);
    cJSON_AddBoolToObject(o, "replaced", replaced);
    cJSON_AddStringToObject(o, "mode", radio_mode_str(radio_mode()));
    emit(o);
    end_ok();
}

static void cmd_reboot(void)
{
    printf("# rebooting\n#END\n");
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void cmd_factory(void)
{
    cfg_factory_reset();
    applog_add(LOG_T_CFG, 0, "factory reset over usb");
    printf("# factory reset, rebooting\n#END\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* ------------------------------- dispatch -------------------------------- */

static void handle_line(char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) return;
    char *a1 = strtok_r(NULL, " \t", &save);

    if      (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
    else if (!strcmp(cmd, "status"))                    cmd_status();
    else if (!strcmp(cmd, "scan"))                      cmd_scan();
    else if (!strcmp(cmd, "profiles"))                  cmd_profiles();
    else if (!strcmp(cmd, "records"))                   cmd_records(a1);
    else if (!strcmp(cmd, "filter"))                    cmd_filter(a1);
    else if (!strcmp(cmd, "timing")) {
        /* sequential strtok_r calls: passing both in one call is an unspecified evaluation
         * order, which silently swapped the arguments */
        char *a2 = strtok_r(NULL, " 	", &save);
        char *a3 = strtok_r(NULL, " 	", &save);
        cmd_scan_cfg(a1, a2, a3);
    }
    else if (!strcmp(cmd, "pins"))                      cmd_pins(a1);
    else if (!strcmp(cmd, "pin"))                       cmd_pin(a1);
    else if (!strcmp(cmd, "unpin"))                     cmd_unpin(a1);
    else if (!strcmp(cmd, "logs"))                      cmd_logs(a1);
    else if (!strcmp(cmd, "mode"))                      { if (a1) cmd_mode(a1); else end_err("mode sniff|clone|idle"); }
    else if (!strcmp(cmd, "use"))                       cmd_use(a1);
    else if (!strcmp(cmd, "del"))                       cmd_del(a1);
    else if (!strcmp(cmd, "clone"))                     cmd_clone(a1);
    else if (!strcmp(cmd, "adv"))                       cmd_adv(a1);
    else if (!strcmp(cmd, "clear"))                     { blescan_clear(); applog_add(LOG_T_SCAN, 0, "usb: table cleared"); end_ok(); }
    else if (!strcmp(cmd, "reboot"))                    cmd_reboot();
    else if (!strcmp(cmd, "factory"))                   { if (a1 && !strcmp(a1, "yes")) cmd_factory(); else end_err("factory yes (erases the config)"); }
    else                                                end_err("unknown command, try 'help'");
}

static void usb_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX_LEN];
    size_t n = 0;

    for (;;) {
        uint8_t buf[64];
        int r = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < r; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (n) {
                    line[n] = 0;
                    n = 0;
                    handle_line(line);
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            } else {
                n = 0;                     /* over-long garbage: drop it */
            }
        }
    }
}

void usb_console_init(void)
{
    /* The driver owns the RX side; making the VFS use it as well keeps `printf` (and every
     * ESP_LOG after this point) on the same path, which is the documented combination. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 512;
    cfg.rx_buffer_size = 1024;
    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s (usb console off)", esp_err_to_name(e));
        return;
    }
    usb_serial_jtag_vfs_use_driver();

    BaseType_t ok = xTaskCreate(usb_task, "usbcmd", TASK_STACK, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed (usb console off)");
        return;
    }
    ESP_LOGI(TAG, "usb console ready: 'help' for commands");
}
