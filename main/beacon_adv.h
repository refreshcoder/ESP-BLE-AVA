#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "app_cfg.h"

/* implemented in main.c */
void      app_on_ble_sync(void);            /* BLE host sync-complete callback */
esp_err_t app_apply_active(void);           /* re-apply the active profile */

esp_err_t beacon_stack_init(void);          /* nimble_port_init + start the host task */
esp_err_t beacon_adv_apply(const beacon_cfg_t *c);
/* stats: advertising start count + current TX power */
void      beacon_adv_stats(uint32_t *starts, int8_t *tx_dbm);
void      beacon_adv_stop(void);
bool      beacon_adv_is_running(void);
