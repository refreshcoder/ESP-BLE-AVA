#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "beacon_scan.h"

/*
 * Capture archive (survives power loss)
 *
 * Location: storage partition (832K, FATFS + wear levelling), file /data/cap.bin
 * Layout  : 12-byte header + 128 fixed-size records (168 B each, ~21.5 KB), ring overwrite
 *
 * Why not JSON/NVS:
 *   - NVS is a small key/value store; it fits neither multi-field records nor ring
 *     overwrite, and frequent writes would chew through the 24K partition
 *   - a fixed-size binary record means a constant write volume (records + the 12 B
 *     header per capture, batched into one pass), no file growth, no garbage
 *     collection, and at worst the newest batch is lost on power failure
 */

#define CAP_SLOTS 128

/* Favourites: records copied out of the archive ring. Kept in their own file so a scan
 * window (which can overwrite all 128 archive slots in one go) never touches them. */
#define PIN_SLOTS 32

typedef struct {
    uint32_t magic;           /* REC_MAGIC: tells a valid record from an empty slot */
    uint32_t boot_seq;        /* which boot this was */
    uint32_t uptime_s;        /* seconds since boot at capture time (no RTC: time = boot_seq + uptime_s) */
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint8_t  adv_type;        /* BLE_HCI_ADV_RPT_EVTYPE_* */

    /* the payload, exactly what a clone would replay */
    uint8_t  raw_len;
    uint8_t  raw[CFG_RAW_MAX];
    uint8_t  rsp_len;         /* scan response, 0 = none captured */
    uint8_t  rsp[CFG_RAW_MAX];

    /* parsed hints, so the archive is readable without re-parsing raw */
    uint16_t mfg_company;     /* SIG company id, LE on air */
    uint16_t svc_uuid;        /* first 16-bit service UUID, 0 = none */
    uint8_t  mfg_len;         /* payload length after the 2-byte company id */
    uint8_t  mfg_src;         /* 0 = advertisement, 1 = scan response */
    uint8_t  mfg[27];
    char     name[16];        /* local name as advertised, NUL padded */

    /* iBeacon, when the payload matched */
    uint8_t  uuid[16];
    uint16_t major, minor;
    int8_t   ref_rssi;

    /* radio statistics over the capture window */
    int8_t   rssi_last, rssi_max, rssi_avg;
    uint16_t itvl_ms;         /* estimated advertising interval */
    uint16_t dwell_ms;        /* last_ms - first_ms, i.e. how long it stayed visible */
    uint8_t  is_ibeacon;      /* 1 = the payload matched iBeacon (uuid/major/minor valid) */
    uint8_t  src;             /* REC_SRC_*: who wrote it (clone action or scan window) */
    uint32_t count;           /* advertising packets seen */
} cap_rec_t;                  /* 168 B, ring of 128 in one 21.5 KB file */

/* where a record came from: a manual clone, or a capture window's snapshot */
#define REC_SRC_CLONE 0
#define REC_SRC_SCAN  1

esp_err_t   storage_init(void);
bool        storage_ready(void);

/* Append one capture (overwrites the oldest slot); boot_seq distinguishes boot sessions */
esp_err_t   storage_capture_add(const detection_t *d, uint32_t boot_seq);

/* Read up to max records, **newest first**; returns the count */
int         storage_capture_list(cap_rec_t *out, int max);

/* Append n captures in one open/write/close pass (a scan window archives up to
 * SCAN_TAB_MAX rows at once; per-record file opens would be needlessly slow).
 * Returns how many records were written. */
int         storage_capture_add_many(const detection_t *d, int n, uint32_t boot_seq, uint8_t src);

esp_err_t   storage_capture_clear(void);

/* Format the partition. Only valid while the archive is NOT mounted (would delete records),
 * i.e. the deliberate escape hatch for a fresh or damaged partition. */
esp_err_t   storage_format(void);

/* ---------------- uploaded SPA (index.html.gz) ----------------
 * The firmware embeds a copy; this is an optional override that lives in the FATFS partition
 * so the UI can be replaced without reflashing. Everything here is best-effort: on any error
 * the caller falls back to the embedded blob. */
#define UI_MIN_BYTES  1024
#define UI_MAX_BYTES  (64 * 1024)

void      storage_ui_prepare(void);
esp_err_t storage_ui_stat(uint32_t *out_crc, size_t *out_len);
/* ESP_OK, or ESP_ERR_NOT_FOUND when nothing is uploaded; ESP_ERR_INVALID_STATE while a stream
 * holds the UI or storage is down, ESP_ERR_NO_MEM when the UI mutex never came up. */
esp_err_t storage_ui_remove(void);

esp_err_t storage_ui_read_begin(void);
int       storage_ui_read_chunk(uint8_t *buf, size_t cap);
/* read_end() and upload_abort() release a session: call them only after a *successful*
 * read_begin()/upload_begin() -- they key on global session state, not on the caller. */
void      storage_ui_read_end(void);

esp_err_t storage_ui_upload_begin(void);
esp_err_t storage_ui_upload_chunk(const void *buf, size_t len);
esp_err_t storage_ui_upload_commit(void);
void      storage_ui_upload_abort(void);

/* --- favourites -------------------------------------------------------------
 * storage_pin_add() copies a record (whole 168 B) into its own file, so it survives both
 * the power switch and any number of later scan windows. */
int         storage_pin_list(cap_rec_t *out, int max);   /* returns the count, oldest first */
esp_err_t   storage_pin_add(const cap_rec_t *r);         /* ESP_ERR_NO_MEM when full */
esp_err_t   storage_pin_remove(int idx);                 /* idx as returned by storage_pin_list */
esp_err_t   storage_pin_clear(void);

/* For /api/status, e.g. "12/32"; returns "off" when unavailable */
const char *storage_stat_str(char *buf, size_t len);
