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
 * Layout  : 12-byte header + 32 fixed-size records, ring-buffer overwrite
 *
 * Why not JSON/NVS:
 *   - NVS is a small key/value store; it fits neither multi-field records nor ring
 *     overwrite, and frequent writes would chew through the 24K partition
 *   - a fixed 70 B binary record means a constant write volume (one record +
 *     the 12 B header per clone), no file growth, no garbage collection, and at
 *     worst the newest record is lost on power failure
 */

#define CAP_SLOTS 128

typedef struct {
    uint32_t magic;          /* 0xCA9E0001: tells a valid record from an empty/garbage slot */
    uint32_t uptime_s;       /* seconds since boot at capture time (no RTC; time = boot_seq + uptime_s) */
    uint32_t boot_seq;       /* which boot this was */
    uint8_t  is_ibeacon;
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint8_t  adv_type;
    uint8_t  raw_len;
    uint8_t  raw[31];
    uint8_t  mfg_len;         /* manufacturer data payload, company id excluded */
    uint8_t  mfg_src;         /* 0 = advertisement, 1 = scan response */
    uint8_t  mfg[27];
    uint16_t mfg_company;
    uint8_t  uuid[16];
    uint16_t major, minor;
    uint16_t itvl_ms;
    int8_t   rssi;
    int8_t   ref_rssi;
} cap_rec_t;                 /* ~100B */

esp_err_t   storage_init(void);
bool        storage_ready(void);

/* Append one capture (overwrites the oldest slot); boot_seq distinguishes boot sessions */
esp_err_t   storage_capture_add(const detection_t *d, uint32_t boot_seq);

/* Read up to max records, **newest first**; returns the count */
int         storage_capture_list(cap_rec_t *out, int max);

esp_err_t   storage_capture_clear(void);

/* Format the partition. Only valid while the archive is NOT mounted (would delete records),
 * i.e. the deliberate escape hatch for a fresh or damaged partition. */
esp_err_t   storage_format(void);

/* For /api/status, e.g. "12/32"; returns "off" when unavailable */
const char *storage_stat_str(char *buf, size_t len);
