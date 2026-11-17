#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * One-tap clone from the capture table.
 *
 * Shared by the HTTP API (`POST /api/clone`) and the USB console (`clone <addr>`), so the
 * two entry points cannot drift apart: profile creation, address handling, the persistent
 * archive write and the radio switch all happen here.
 *
 * Errors:
 *   ESP_ERR_NOT_FOUND     - address is not in the scan table
 *   ESP_ERR_INVALID_STATE - nothing to clone (no payload) or no free/evictable slot
 *   other                 - cfg_save()/radio_clone() failure
 * `msg` always carries a human-readable reason on failure.
 */
esp_err_t clone_op_from_addr(const uint8_t addr[6], int *out_id, uint16_t *out_itvl,
                             bool *out_replaced, char *msg, size_t msglen);
