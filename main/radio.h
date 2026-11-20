#pragma once
#include <stdbool.h>
#include "esp_err.h"

/*
 * RF mode arbiter. SNIFF (capture) and CLONE (advertise) are mutually exclusive:
 * only one of them runs at a time. Every mode change must go through this module
 * so that "start advertising without stopping the scan" (and the reverse) cannot
 * happen.
 */

typedef enum {
    RMODE_IDLE = 0,   /* radio off, only the SoftAP/UI is up */
    RMODE_SNIFF,      /* scan only, records raw advertising data */
    RMODE_CLONE,      /* advertise only, replays the active profile */
} rmode_t;

esp_err_t   radio_init(void);

esp_err_t   radio_sniff(void);   /* stop advertising -> start scanning (60 s, then idle) */
esp_err_t   radio_clone(void);   /* stop scanning -> advertise the active profile */
esp_err_t   radio_stop(void);    /* stop both */

rmode_t     radio_mode(void);
const char *radio_mode_str(rmode_t m);
bool        radio_sniff_timed_out(void);   /* manual sniff window elapsed */

/* Called once, right before the radio leaves SNIFF mode by any path (window timeout,
 * button, web UI, console). main() uses it to archive the capture table, so a table is
 * never lost just because the operator stopped the scan early. Must not call back into
 * radio_*(). */
void        radio_set_sniff_end_cb(void (*cb)(void));

/* Called by main once the BLE host is synced: picks the initial mode */
void        radio_on_ble_ready(void);