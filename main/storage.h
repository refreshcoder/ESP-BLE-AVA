#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "beacon_scan.h"

/*
 * 捕获归档（掉电保留）
 *
 * 位置：storage 分区（832K，FATFS + 磨损均衡），文件 /data/cap.bin
 * 结构：12 字节头 + 32 个定长记录，环形覆盖写
 * 为什么不用 JSON/NVS：
 *   - NVS 是小键值库，不适合"整段记录 + 环形覆盖"，且频繁写会吃掉 24K 分区
 *   - 定长二进制 50B/条，写入量固定（每次点"复制"写 1 条 + 12B 头），
 *     不产生文件增长/GC，掉电最坏只丢最后一条
 */

#define CAP_SLOTS 32

typedef struct {
    uint32_t magic;          /* 0xCA9E0001，用于识别空槽/垃圾 */
    uint32_t uptime_s;       /* 捕获时的运行秒数（设备无 RTC，用启动序号+运行时长表达时间） */
    uint32_t boot_seq;       /* 第几次启动 */
    uint8_t  is_ibeacon;
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint8_t  adv_type;
    uint8_t  raw_len;
    uint8_t  raw[31];
    uint8_t  uuid[16];
    uint16_t major, minor;
    uint16_t itvl_ms;
    int8_t   rssi;
    int8_t   ref_rssi;
} cap_rec_t;                 /* ~70B */

esp_err_t   storage_init(void);
bool        storage_ready(void);

/* 追加一条捕获（环形覆盖最旧），boot_seq 用于区分不同启动批次 */
esp_err_t   storage_capture_add(const detection_t *d, uint32_t boot_seq);

/* 读出最多 max 条，**新的在前**；返回条数 */
int         storage_capture_list(cap_rec_t *out, int max);

esp_err_t   storage_capture_clear(void);

/* 供 /api/status 显示，如 "12/32"，失败返回 "off" */
const char *storage_stat_str(char *buf, size_t len);
