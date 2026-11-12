#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define CFG_MAX_PROFILES 3    /* product limit: 3 slots; the least-recently-used one is evicted when full */
#define CFG_UUID_LEN     16
#define CFG_NAME_MAX     24
#define CFG_SSID_MAX     32
#define CFG_PASS_MAX     63
#define CFG_RAW_MAX      31    /* legacy advertising data limit */

/* replication kind */
#define PROFILE_KIND_IBEACON 0 /* structured iBeacon (UUID/Major/Minor rebuilt) */
#define PROFILE_KIND_RAW     1 /* verbatim bytes (replicates any BLE advertisement) */

/* one beacon profile */
typedef struct {
    char     name[CFG_NAME_MAX];
    bool     enabled;
    uint8_t  kind;            /* PROFILE_KIND_* */

    /* ---- kind = IBEACON ---- */
    uint8_t  uuid[CFG_UUID_LEN];
    uint16_t major;
    uint16_t minor;
    int8_t   ref_rssi;        /* iBeacon byte 30: calibrated RSSI at 1 m */

    /* ---- kind = RAW ---- */
    uint8_t  raw[CFG_RAW_MAX];
    uint8_t  raw_len;

    /* ---- common ---- */
    int8_t   radio_dbm;       /* radio TX power, dBm */
    uint16_t adv_itvl_ms;     /* advertising interval, 20..10240 ms */
    bool     scannable;       /* true = ADV_SCAN_IND, false = ADV_NONCONN_IND */
    bool     use_rand_addr;
    uint8_t  rand_addr[6];
    uint8_t  src_addr[6];     /* provenance: address this profile was copied from */
    uint8_t  src_adv_type;    /* source adv event type, BLE_HCI_ADV_RPT_EVTYPE_* */

    uint32_t use_seq;         /* monotonic stamp of last activation/overwrite (LRU key) */
} beacon_profile_t;

typedef struct {
    bool     enabled;
    uint16_t itvl_ms;
    uint16_t win_ms;
} scan_cfg_t;

typedef struct {
    char ssid[CFG_SSID_MAX + 1];
    char pass[CFG_PASS_MAX + 1];
} ap_cfg_t;

typedef struct {
    uint16_t ver;
    int      count;
    int      active;          /* -1 = nothing active */
    bool     rotation;
    uint16_t rotation_s;
    uint32_t seq_gen;         /* use_seq generator, persisted with the config */
    scan_cfg_t scan;
    ap_cfg_t   ap;
    beacon_profile_t p[CFG_MAX_PROFILES];
} beacon_cfg_t;

esp_err_t      cfg_init(void);
void           cfg_lock(void);
void           cfg_unlock(void);
beacon_cfg_t  *cfg_get(void);
esp_err_t      cfg_save(void);
esp_err_t      cfg_factory_reset(void);
void           cfg_defaults(beacon_cfg_t *c);
int            cfg_free_slot(const beacon_cfg_t *c);
/* Pick a slot to (over)write: a free slot first, else the smallest use_seq that is not active.
 * Returns -1 when only the active profile is left. */
int            cfg_pick_slot(beacon_cfg_t *c);
/* Stamp a fresh use_seq on a profile (caller holds the lock) */
void           cfg_touch(beacon_cfg_t *c, int idx);

/* JSON (de)serialisation, used by the web layer */
struct cJSON;
struct cJSON *cfg_to_json(const beacon_cfg_t *c);
bool          cfg_from_json(const char *js, beacon_cfg_t *c, char *err, size_t errlen);
bool          cfg_profile_from_json(const struct cJSON *j, beacon_profile_t *p, char *err, size_t errlen);
void          cfg_profile_to_json(const beacon_profile_t *p, struct cJSON *dst);

bool           cfg_uuid_from_str(const char *s, uint8_t out[CFG_UUID_LEN]);
void           cfg_uuid_to_str(const uint8_t u[CFG_UUID_LEN], char out[33]);
void           cfg_hex_to_str(const uint8_t *in, int n, char *out);   /* out must hold 2n+1 chars */
bool           cfg_hex_from_str(const char *s, uint8_t *out, int n);
const char    *cfg_api_token(void);

/* Lightweight NVS counters (boots, brownouts, ...) for serial-free diagnostics */
uint32_t       cfg_stat_get(const char *key);
void           cfg_stat_bump(const char *key, uint32_t add);