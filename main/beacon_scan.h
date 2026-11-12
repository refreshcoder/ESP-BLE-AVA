#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_cfg.h"

#define SCAN_TAB_MAX  16
#define SCAN_RAW_MAX  CFG_RAW_MAX

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint8_t  adv_type;         /* BLE_HCI_ADV_RPT_EVTYPE_* */
    uint8_t  raw_len;
    uint8_t  raw[SCAN_RAW_MAX];/* 原始 advertising data（照搬用） */

    /* iBeacon 解析（若命中） */
    bool     is_ibeacon;
    uint8_t  uuid[16];
    uint16_t major, minor;
    int8_t   ref_rssi;

    /* 统计 */
    int8_t   rssi_last, rssi_max, rssi_avg;
    uint32_t count;
    uint32_t first_ms, last_ms;
    uint16_t itvl_ms;          /* 估计广播间隔 */
    uint32_t last_log_ms;
    char     name[16];
} detection_t;

esp_err_t blescan_init(void);
esp_err_t blescan_start(const scan_cfg_t *sc);
void      blescan_stop(void);
bool      blescan_running(void);

/* 统计：本次扫描累计收到的广播包数 + 已运行时长(ms) */
void      blescan_stats(uint32_t *pkts, uint32_t *running_ms);
int       blescan_count(void);
int       blescan_snapshot(detection_t *out, int max);  /* 按 last_ms 倒序 */
bool      blescan_find(const uint8_t addr[6], detection_t *out);
void      blescan_clear(void);