#pragma once
#include <stdbool.h>
#include "esp_err.h"

/*
 * 射频模式仲裁：捕获(SNIFF) 与 广播(CLONE) 互斥，同一时刻只有一种在跑。
 * 所有模式切换都必须经过本模块，避免"扫描没停就去广播"或反过来的竞态。
 */

typedef enum {
    RMODE_IDLE = 0,   /* 射频空闲（只开 AP 管理页） */
    RMODE_SNIFF,      /* 扫监听，记录原始广播数据 */
    RMODE_CLONE,      /* 广播复刻当前 active 配置 */
} rmode_t;

esp_err_t   radio_init(void);

esp_err_t   radio_sniff(void);   /* 停广播 → 开扫描 */
esp_err_t   radio_clone(void);   /* 停扫描 → 按 active 配置广播 */
esp_err_t   radio_stop(void);    /* 两者都停 */

rmode_t     radio_mode(void);
const char *radio_mode_str(rmode_t m);

/* BLE host 同步完成后由 main 调用：按配置决定首个模式 */
void        radio_on_ble_ready(void);