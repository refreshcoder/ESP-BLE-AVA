#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_cfg.h"

#define SCAN_TAB_MAX  128
#define SCAN_RAW_MAX  CFG_RAW_MAX

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint8_t  adv_type;         /* BLE_HCI_ADV_RPT_EVTYPE_* */
    uint8_t  raw_len;
    uint8_t  raw[SCAN_RAW_MAX];/* raw advertising data, replayed verbatim */
    uint8_t  rsp_len;          /* scan response length, 0 = none captured */
    uint8_t  rsp[SCAN_RAW_MAX];/* raw scan response, replayed verbatim */

    /* manufacturer specific data (AD type 0xFF), first occurrence seen.
     * Many devices put this in the scan response rather than the advertisement,
     * which is why mfg_src records where it came from. */
    uint16_t mfg_company;      /* company identifier as it appears on air (LE) */
    uint8_t  mfg_len;          /* payload length after the 2-byte company id */
    uint8_t  mfg[27];
    uint8_t  mfg_src;          /* 0 = advertisement, 1 = scan response */
    uint16_t svc_uuid;         /* first 16-bit service UUID seen (0 = none): tells the
                                * ecosystem for vendors that put everything in service data */

    /* iBeacon fields, when the payload matches */
    bool     is_ibeacon;
    uint8_t  uuid[16];
    uint16_t major, minor;
    int8_t   ref_rssi;

    /* statistics */
    int8_t   rssi_last, rssi_max, rssi_avg;
    uint32_t count;
    uint32_t first_ms, last_ms;
    uint16_t itvl_ms;          /* estimated advertising interval */
    uint32_t last_log_ms;
    char     name[16];
} detection_t;

esp_err_t blescan_init(void);
esp_err_t blescan_start(const scan_cfg_t *sc);
void      blescan_stop(void);
bool      blescan_running(void);

/* stats: packets received in this scan + elapsed ms */
void      blescan_stats(uint32_t *pkts, uint32_t *running_ms);
int       blescan_count(void);
int       blescan_snapshot(detection_t *out, int max);  /* newest first, by last_ms */
bool      blescan_find(const uint8_t addr[6], detection_t *out);
void      blescan_clear(void);

/* "Apple" for 0x004C, "0x027D" when unknown; never NULL */
const char *blescan_company_name(uint16_t id);

/* "Fast Pair" for 0xFE2C, "0xFCB2" when unknown; never NULL */
const char *blescan_service_name(uint16_t uuid);

/* Vendor guessed from the advertised local name, for devices that carry no company id
 * (most attendance/access-control terminals). NULL when nothing matches. */
const char *blescan_name_vendor(const char *name);