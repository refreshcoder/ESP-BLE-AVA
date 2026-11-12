#pragma once

/*
 * board.h -- board definitions. Change BOARD_ID, or switch at build time:
 *   idf.py -DAPP_BOARD=2 build
 *
 *   1 = Espressif ESP32-C6-DevKitC-1  (WS2812 RGB on GPIO8, BOOT on GPIO9)
 *   2 = Seeed XIAO ESP32-C6           (mono LED GPIO15, BOOT on GPIO9)
 *   3 = Waveshare ESP32-C6-Zero       (verify silkscreen before trusting)
 */

#define BOARD_DEVKITC1      1
#define BOARD_XIAO_C6       2
#define BOARD_WAVESHARE_ZERO 3

#ifndef BOARD_ID
#define BOARD_ID            BOARD_XIAO_C6   /* actual board; use BOARD_DEVKITC1 for the devkit */
#endif

/* ---------------- button: BOOT on GPIO9, active low on all boards ---------------- */
#define BOARD_BTN_GPIO          9
#define BOARD_BTN_ACTIVE_LEVEL  0

#define BOARD_BTN_SHORT_MS      60     /* shortest valid click (debounce) */
#define BOARD_BTN_SHORT_MAX_MS  1200   /* longer than this but < long press = ignored */
#define BOARD_BTN_LONG_MS       5000   /* long press: factory reset */
#define BOARD_BTN_MULTI_MS      450    /* double-click window */

/* ---------------- LED ---------------- */
#define LED_BRIGHT_MAX          40     /* 0..255; keep the indicator dim */

/* BLE TX power range (C6: -15..+20 dBm, 3 dB steps) */
#define BOARD_BLE_TX_MIN_DBM   (-15)
#define BOARD_BLE_TX_MAX_DBM   (20)

#if BOARD_ID == BOARD_XIAO_C6
/* ---- Seeed XIAO ESP32-C6 ---- */
#define BOARD_NAME            "XIAO ESP32-C6"
#define BOARD_LED_GPIO        15
#define BOARD_LED_WS2812      0        /* single-colour user LED, not WS2812 */
#define BOARD_LED_ACTIVE_LEVEL 1       /* flip to 0 if the LED stays dark */

#elif BOARD_ID == BOARD_WAVESHARE_ZERO
/* ---- Waveshare ESP32-C6-Zero (verify with schematic/silkscreen) ---- */
#define BOARD_NAME            "Waveshare ESP32-C6-Zero"
#define BOARD_LED_GPIO        15
#define BOARD_LED_WS2812      0
#define BOARD_LED_ACTIVE_LEVEL 1

#else
/* ---- Espressif ESP32-C6-DevKitC-1 ---- */
#define BOARD_NAME            "ESP32-C6-DevKitC-1"
#define BOARD_LED_GPIO        8
#define BOARD_LED_WS2812      1
#define BOARD_LED_ACTIVE_LEVEL 1

#endif
