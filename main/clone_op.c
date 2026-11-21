#include "clone_op.h"
#include <string.h>
#include <stdio.h>
#include "app_cfg.h"
#include "app_log.h"
#include "beacon_scan.h"
#include "radio.h"
#include "storage.h"

#define ADDR_TYPE_RANDOM 1   /* BLE_ADDR_RANDOM; keeps nimble headers out of this layer */

esp_err_t clone_op_from_addr(const uint8_t addr[6], int *out_id, uint16_t *out_itvl,
                             bool *out_replaced, char *msg, size_t msglen)
{
    detection_t d;
    if (!blescan_find(addr, &d)) {
        snprintf(msg, msglen, "not in capture table, run a scan first");
        return ESP_ERR_NOT_FOUND;
    }
    if (d.raw_len < 2) {
        snprintf(msg, msglen, "no payload captured (try active scan)");
        return ESP_ERR_INVALID_STATE;
    }

    int id = -1;
    bool replaced = false;
    uint16_t itvl = d.itvl_ms;
    if (itvl < 20)    itvl = 100;      /* only one packet seen: conservative default */
    if (itvl > 10240) itvl = 10240;

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    int slot = cfg_pick_slot(c);       /* free slot first, else evict the LRU idle profile */
    if (slot >= 0) {
        replaced = (slot < c->count);
        beacon_profile_t *p = &c->p[slot];
        memset(p, 0, sizeof(*p));
        if (d.is_ibeacon)
            snprintf(p->name, sizeof(p->name), "copy-%u/%u", d.major, d.minor);
        else if (d.name[0])
            snprintf(p->name, sizeof(p->name), "copy-%.11s", d.name);
        else
            snprintf(p->name, sizeof(p->name), "copy-%02X%02X%02X", addr[3], addr[4], addr[5]);

        p->kind     = PROFILE_KIND_RAW;     /* verbatim: replicating anything */
        p->enabled  = true;
        memcpy(p->raw, d.raw, d.raw_len);
        p->raw_len  = d.raw_len;
        memcpy(p->rsp, d.rsp, d.rsp_len);
        p->rsp_len  = d.rsp_len;            /* 0 when no SCAN_RSP was captured */
        memcpy(p->src_addr, addr, 6);
        p->src_adv_type = d.adv_type;
        if (d.is_ibeacon) {
            memcpy(p->uuid, d.uuid, 16);
            p->major    = d.major;
            p->minor    = d.minor;
            p->ref_rssi = d.ref_rssi;
        } else {
            p->ref_rssi = -59;
        }
        p->radio_dbm   = 9;
        p->adv_itvl_ms = itvl;
        /* Keep scannability when the source was ADV_IND/ADV_SCAN_IND, otherwise
         * advertise non-connectable and non-scannable. A captured scan response implies
         * the source answered SCAN_REQ, so it was scannable. */
        p->scannable   = (d.adv_type == 0x00 || d.adv_type == 0x02) || d.rsp_len > 0;
        /* Address: a random address clones exactly (same bytes, top two bits forced to 11).
         * A public address cannot be chosen per advertising instance, but it can be set for
         * the whole boot with esp_iface_mac_addr_set(ESP_MAC_BT) -- app_main does that for the
         * active profile, and radio_clone() reboots when a switch needs another one. */
        if (d.addr_type == ADDR_TYPE_RANDOM) {
            memcpy(p->rand_addr, addr, 6);
            p->rand_addr[0] |= 0xC0;          /* display order: top two bits of the first octet */
            p->use_rand_addr = true;
        } else {
            p->use_rand_addr = false;
            p->clone_public_addr = true;      /* takes effect after the reboot below */
        }

        if (!replaced) c->count++;
        c->active = slot;
        cfg_touch(c, slot);
        id = slot;
        cfg_save();
        applog_add(LOG_T_CFG, d.rssi_last, "clone %02x%02x%02x%02x%02x%02x (%s) -> #%d raw%d itvl%ums",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                   d.addr_type == ADDR_TYPE_RANDOM ? "random, pinned as static" : "public, applied at boot",
                   id, d.raw_len, (unsigned)itvl);
    }
    cfg_unlock();

    if (id < 0) {
        snprintf(msg, msglen, "config full and only the active profile is left; delete one first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Persistent archive: exactly one write here, so the rate is negligible */
    storage_capture_add(&d, cfg_stat_get("boots"));

    esp_err_t e = radio_clone();
    if (e != ESP_OK) {
        snprintf(msg, msglen, "clone apply failed");
        return e;
    }

    if (out_id)       *out_id = id;
    if (out_itvl)     *out_itvl = itvl;
    if (out_replaced) *out_replaced = replaced;
    return ESP_OK;
}

esp_err_t clone_op_from_raw(const uint8_t *raw, uint8_t raw_len, const uint8_t *rsp, uint8_t rsp_len,
                            int *out_id, uint16_t *out_itvl, bool *out_replaced,
                            char *msg, size_t msglen)
{
    if (!raw || raw_len < 2 || raw_len > CFG_RAW_MAX) {
        snprintf(msg, msglen, "raw must be 2..%d bytes", CFG_RAW_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (rsp_len > CFG_RAW_MAX) {
        snprintf(msg, msglen, "rsp must be 0..%d bytes", CFG_RAW_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t itvl = 100;         /* the sources we have seen advertise at ~82..100 ms */
    int id = -1;
    bool replaced = false;

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    int slot = -1;
    for (int i = 0; i < c->count; i++) {
        if (!strcmp(c->p[i].name, "usb-raw")) { slot = i; break; }
    }
    if (slot < 0) slot = cfg_pick_slot(c);
    if (slot >= 0) {
        replaced = (slot < c->count);
        beacon_profile_t *p = &c->p[slot];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "usb-raw");
        p->kind        = PROFILE_KIND_RAW;
        p->enabled     = true;
        memcpy(p->raw, raw, raw_len);
        p->raw_len     = raw_len;
        if (rsp && rsp_len) {
            memcpy(p->rsp, rsp, rsp_len);
            p->rsp_len = rsp_len;
        }
        p->ref_rssi    = -59;
        p->radio_dbm   = 9;
        p->adv_itvl_ms = itvl;
        p->scannable   = rsp_len > 0;  /* a scan response implies the source answered SCAN_REQ */

        if (!replaced) c->count++;
        c->active = slot;
        cfg_touch(c, slot);
        id = slot;
        cfg_save();
        applog_add(LOG_T_CFG, 0, "raw profile #%d: %u bytes", id, (unsigned)raw_len);
    }
    cfg_unlock();

    if (id < 0) {
        snprintf(msg, msglen, "config full and only the active profile is left; delete one first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t e = radio_clone();
    if (e != ESP_OK) {
        snprintf(msg, msglen, "clone apply failed");
        return e;
    }

    if (out_id)       *out_id = id;
    if (out_itvl)     *out_itvl = itvl;
    if (out_replaced) *out_replaced = replaced;
    return ESP_OK;
}

esp_err_t profile_delete(int id, bool *out_stopped)
{
    if (out_stopped) *out_stopped = false;

    cfg_lock();
    beacon_cfg_t *c = cfg_get();
    if (id < 0 || id >= c->count) {
        cfg_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = id; i < c->count - 1; i++) c->p[i] = c->p[i + 1];
    c->count--;

    bool stop = false;
    if (c->active == id)     { c->active = -1; stop = true; }
    else if (c->active > id) c->active--;
    cfg_save();
    cfg_unlock();

    applog_add(LOG_T_CFG, 0, "profile %d deleted", id);
    if (out_stopped) *out_stopped = stop;
    return ESP_OK;
}
