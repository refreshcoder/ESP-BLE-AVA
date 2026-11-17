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
           "# logs [since_seq]              - JSONL: ring log records\n"
           "# mode sniff|clone|idle         - switch the radio (arbitrated by radio.c)\n"
           "# use <id>|-1                   - activate a profile / broadcast nothing\n"
           "# clone <addr 12 hex>           - copy a row of the capture table into a profile\n"
           "# clear                         - empty the capture table\n"
           "# records clear                 - empty the persistent archive\n"
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

static void cmd_records(int clear)
{
    if (clear) {
        esp_err_t e = storage_capture_clear();
        if (e != ESP_OK) { end_err("archive unavailable"); return; }
        end_ok();
        return;
    }

    if (!storage_ready()) { end_err("archive unavailable"); return; }
    cap_rec_t *recs = calloc(CAP_SLOTS, sizeof(cap_rec_t));
    if (!recs) { end_err("oom"); return; }
    int n = storage_capture_list(recs, CAP_SLOTS);
    char h[SCAN_RAW_MAX * 2 + 1];

    for (int i = 0; i < n; i++) {
        const cap_rec_t *r = &recs[i];
        cJSON *o = cJSON_CreateObject();
        cfg_hex_to_str(r->addr, 6, h);
        cJSON_AddStringToObject(o, "addr", h);
        cJSON_AddNumberToObject(o, "adv_type", r->adv_type);
        cJSON_AddNumberToObject(o, "rssi", r->rssi);
        cJSON_AddNumberToObject(o, "itvl_ms", r->itvl_ms);
        cJSON_AddNumberToObject(o, "raw_len", r->raw_len);
        cfg_hex_to_str(r->raw, r->raw_len, h);
        cJSON_AddStringToObject(o, "raw", h);
        if (r->mfg_len || r->mfg_company) {
            char mh[sizeof(r->mfg) * 2 + 1];
            cfg_hex_to_str(r->mfg, r->mfg_len, mh);
            cJSON_AddNumberToObject(o, "mfg_company", r->mfg_company);
            cJSON_AddNumberToObject(o, "mfg_src", r->mfg_src);
            cJSON_AddStringToObject(o, "mfg", mh);
        }
        cJSON_AddNumberToObject(o, "boot_seq", r->boot_seq);
        cJSON_AddNumberToObject(o, "uptime_s", r->uptime_s);
        emit(o);
    }
    free(recs);
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
    else if (!strcmp(cmd, "records"))                   cmd_records(a1 && !strcmp(a1, "clear"));
    else if (!strcmp(cmd, "logs"))                      cmd_logs(a1);
    else if (!strcmp(cmd, "mode"))                      { if (a1) cmd_mode(a1); else end_err("mode sniff|clone|idle"); }
    else if (!strcmp(cmd, "use"))                       cmd_use(a1);
    else if (!strcmp(cmd, "clone"))                     cmd_clone(a1);
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
