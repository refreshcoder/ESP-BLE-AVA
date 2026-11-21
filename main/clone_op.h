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

/*
 * The same thing without the capture table: build the profile from bytes the caller already
 * has (a `raw` hex string typed on the USB console). Needed when the source is out of radio
 * range -- the payload was captured earlier and only needs to be replayed.
 *
 * The profile is named `usb-raw` and reused on every call, so repeating the command updates
 * one slot instead of filling the config up.
 */
esp_err_t clone_op_from_raw(const uint8_t *raw, uint8_t raw_len, const uint8_t *rsp, uint8_t rsp_len,
                            int *out_id, uint16_t *out_itvl, bool *out_replaced,
                            char *msg, size_t msglen);

/*
 * Delete profile <id> and close the gap, keeping `active` pointing at the same profile.
 * Shared by HTTP `DELETE /api/profiles/<id>` and the USB console (`del <id>`) for the same
 * reason the clone op is shared: one behaviour, two front ends.
 *
 * `out_stopped` is true when the deleted profile was the one on air; the caller then stops
 * the radio (this layer does not own mode transitions).
 *
 * Errors: ESP_ERR_NOT_FOUND when the id does not exist.
 */
esp_err_t profile_delete(int id, bool *out_stopped);
