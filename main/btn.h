#pragma once

/*
 * Single-button (BOOT / GPIO9) user interface:
 *   single click  -> toggle sniff <-> clone (primary action)
 *   double click  -> stop the radio (idle)
 *   long press    -> factory reset + reboot (>=5 s, the physical escape hatch)
 *
 * The XIAO ESP32-C6 exposes only one usable button (the other is RESET and is
 * invisible to firmware), so three gestures cover the whole feature set.
 */
typedef void (*btn_single_cb_t)(void);
typedef void (*btn_double_cb_t)(void);
typedef void (*btn_long_cb_t)(void);

void btn_init(btn_single_cb_t on_single, btn_double_cb_t on_double, btn_long_cb_t on_long);
