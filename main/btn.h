#pragma once

/*
 * Single-button (BOOT / GPIO9) user interface:
 *   single click  -> toggle clone on/off (primary action)
 *   double click  -> stop the radio (idle)
 *   hold 5 s      -> warning only: LED switches to the fast-blink pattern, nothing is erased
 *   hold 8 s      -> factory reset + reboot (the physical escape hatch)
 *   release 5..8 s-> cancel; that window is what keeps a fumbled press from wiping the config
 *
 * The XIAO ESP32-C6 exposes only one usable button (the other is RESET and is
 * invisible to firmware), so three gestures cover the whole feature set.
 */
typedef void (*btn_single_cb_t)(void);
typedef void (*btn_double_cb_t)(void);

typedef enum {
    BTN_HOLD_WARN = 0,   /* held 5 s: announce it, destroy nothing yet */
    BTN_HOLD_CANCEL,     /* released before 8 s: abort */
    BTN_HOLD_COMMIT,     /* held 8 s: do it */
} btn_hold_t;

typedef void (*btn_hold_cb_t)(btn_hold_t ev);

void btn_init(btn_single_cb_t on_single, btn_double_cb_t on_double, btn_hold_cb_t on_hold);
