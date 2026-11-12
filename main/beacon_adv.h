#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "app_cfg.h"

/* 以下由 main.c 实现 */
void      app_on_ble_sync(void);            /* BLE host 同步完成回调 */
esp_err_t app_apply_active(void);           /* 重新应用 active 配置 + 扫描参数 */

esp_err_t beacon_stack_init(void);          /* nimble_port_init + 启动 host 任务 */
esp_err_t beacon_adv_apply(const beacon_cfg_t *c);
/* 统计：累计启动广播次数 + 当前射频功率 */
void      beacon_adv_stats(uint32_t *starts, int8_t *tx_dbm);
void      beacon_adv_stop(void);
bool      beacon_adv_is_running(void);
