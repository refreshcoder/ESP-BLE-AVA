#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define CFG_MAX_PROFILES 3    /* 产品定为 3 套；满了覆盖最久未用的一套 */
#define CFG_UUID_LEN     16
#define CFG_NAME_MAX     24
#define CFG_SSID_MAX     32
#define CFG_PASS_MAX     63
#define CFG_RAW_MAX      31    /* legacy advertising data 上限 */

/* 复刻方式 */
#define PROFILE_KIND_IBEACON 0 /* 结构化 iBeacon（UUID/Major/Minor 重建） */
#define PROFILE_KIND_RAW     1 /* 原始字节照搬（任意 BLE 广播都能复刻） */

/* 一套信标配置 */
typedef struct {
    char     name[CFG_NAME_MAX];
    bool     enabled;
    uint8_t  kind;            /* PROFILE_KIND_* */

    /* ---- kind = IBEACON ---- */
    uint8_t  uuid[CFG_UUID_LEN];
    uint16_t major;
    uint16_t minor;
    int8_t   ref_rssi;        /* iBeacon 第 30 字节: 1m 处标定 RSSI */

    /* ---- kind = RAW ---- */
    uint8_t  raw[CFG_RAW_MAX];
    uint8_t  raw_len;

    /* ---- 公共 ---- */
    int8_t   radio_dbm;       /* 射频发射功率 dBm */
    uint16_t adv_itvl_ms;     /* 广播间隔 20..10240 */
    bool     scannable;       /* true = ADV_SCAN_IND, false = ADV_NONCONN_IND */
    bool     use_rand_addr;
    uint8_t  rand_addr[6];
    uint8_t  src_addr[6];     /* 拷贝来源地址（仅溯源用） */
    uint8_t  src_adv_type;    /* 来源广播类型 BLE_HCI_ADV_RPT_EVTYPE_* */

    uint32_t use_seq;         /* 最近一次被启用/覆盖时的单调序号（判"最久未用"） */
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
    int      active;          /* -1 = 无激活配置 */
    bool     rotation;
    uint16_t rotation_s;
    uint32_t seq_gen;         /* use_seq 发生器，随配置一起持久化 */
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
/* 找一个"可覆盖"的槽位：优先空槽，其次 use_seq 最小且非 active 的；返回 -1 表示无（只剩 active） */
int            cfg_pick_slot(beacon_cfg_t *c);
/* 给 profile 打上新的 use_seq（调用方持锁） */
void           cfg_touch(beacon_cfg_t *c, int idx);

/* JSON 序列化/反序列化（web 层用） */
struct cJSON;
struct cJSON *cfg_to_json(const beacon_cfg_t *c);
bool          cfg_from_json(const char *js, beacon_cfg_t *c, char *err, size_t errlen);
bool          cfg_profile_from_json(const struct cJSON *j, beacon_profile_t *p, char *err, size_t errlen);
void          cfg_profile_to_json(const beacon_profile_t *p, struct cJSON *dst);

bool           cfg_uuid_from_str(const char *s, uint8_t out[CFG_UUID_LEN]);
void           cfg_uuid_to_str(const uint8_t u[CFG_UUID_LEN], char out[33]);
void           cfg_hex_to_str(const uint8_t *in, int n, char *out);   /* out 需 2n+1 */
bool           cfg_hex_from_str(const char *s, uint8_t *out, int n);
const char    *cfg_api_token(void);

/* 轻量 NVS 计数器（启动次数 / 掉压次数等，用于无串口诊断） */
uint32_t       cfg_stat_get(const char *key);
void           cfg_stat_bump(const char *key, uint32_t add);