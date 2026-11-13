#pragma once

#include "sdkconfig.h"   /* CONFIG_IDF_TARGET_* drives the default preset */

/*
 * board.h -- board definitions. Change BOARD_ID, or switch at build time:
 *   idf.py -DAPP_BOARD=2 build
 *
 * ESP32-C6 boards
 *   1 = Espressif ESP32-C6-DevKitC-1  (WS2812 RGB on GPIO8, BOOT on GPIO9)
 *   2 = Seeed XIAO ESP32-C6           (mono LED GPIO15, BOOT on GPIO9)   [default on C6]
 *   3 = Waveshare ESP32-C6-Zero       (verify silkscreen before trusting)
 *
 * ESP32-C3 boards
 *   4 = ESP32-C3 SuperMini            (mono LED GPIO8, BOOT on GPIO9)   [default on C3]
 *   5 = Espressif ESP32-C3-DevKitM-1  (WS2812 RGB on GPIO8, BOOT on GPIO9)
 *
 * When BOARD_ID is not given it follows the build target, so the same tree builds
 * for both chips without editing anything.
 */

#define BOARD_DEVKITC1       1
#define BOARD_XIAO_C6        2
#define BOARD_WAVESHARE_ZERO 3
#define BOARD_C3_SUPERMINI   4
#define BOARD_C3_DEVKITM1    5

#ifndef BOARD_ID
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define BOARD_ID             BOARD_C3_SUPERMINI
#else
#define BOARD_ID             BOARD_XIAO_C6
#endif
#endif

/* Nudge: a C6 preset on a C3 target (or the reverse) usually means a stale -DAPP_BOARD. */
#if defined(CONFIG_IDF_TARGET_ESP32C3) && (BOARD_ID == BOARD_DEVKITC1 || BOARD_ID == BOARD_XIAO_C6 || BOARD_ID == BOARD_WAVESHARE_ZERO)
#warning "BOARD_ID is a ESP32-C6 preset but the target is ESP32-C3 (see board.h)"
#elif defined(CONFIG_IDF_TARGET_ESP32C6) && (BOARD_ID == BOARD_C3_SUPERMINI || BOARD_ID == BOARD_C3_DEVKITM1)
#warning "BOARD_ID is an ESP32-C3 preset but the target is ESP32-C6 (see board.h)"
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

/* BLE TX power range (-15..+20 dBm, 3 dB steps; the chip rejects unsupported levels) */
#define BOARD_BLE_TX_MIN_DBM   (-15)
#define BOARD_BLE_TX_MAX_DBM   (20)

#if BOARD_ID == BOARD_C3_SUPERMINI
/* ---- ESP32-C3 SuperMini (and most C3 "SuperMini"/Zero clones) ---- */
#define BOARD_NAME            "ESP32-C3 SuperMini"
#define BOARD_LED_GPIO        8
#define BOARD_LED_WS2812      0        /* plain single-colour LED, usually active low */
#define BOARD_LED_ACTIVE_LEVEL 0       /* flip to 1 if the LED stays dark */

#elif BOARD_ID == BOARD_C3_DEVKITM1
/* ---- Espressif ESP32-C3-DevKitM-1 ---- */
#define BOARD_NAME            "ESP32-C3-DevKitM-1"
#define BOARD_LED_GPIO        8
#define BOARD_LED_WS2812      1        /* addressable RGB */
#define BOARD_LED_ACTIVE_LEVEL 1

#elif BOARD_ID == BOARD_XIAO_C6
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
