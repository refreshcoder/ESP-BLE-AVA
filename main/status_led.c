#include "status_led.h"
#include "board.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#if BOARD_LED_WS2812
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#endif

static const char *TAG = "led";
static volatile led_state_t s_state = LED_NO_CFG;

static const char *k_state[] = { "no_cfg", "idle", "clone", "clone+client", "sniff", "error" };

const char *led_state_str(led_state_t st)
{
    return st <= LED_ERROR ? k_state[st] : "?";
}

/* ------------------------- hardware layer ------------------------- */

#if BOARD_LED_WS2812

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

static esp_err_t ws2812_init(void)
{
    rmt_tx_channel_config_t c = {
        .gpio_num          = BOARD_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,   /* 0.1us / tick */
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    esp_err_t e = rmt_new_tx_channel(&c, &s_chan);
    if (e != ESP_OK) return e;

    rmt_bytes_encoder_config_t bec = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags = { .msb_first = 1 },
    };
    e = rmt_new_bytes_encoder(&bec, &s_enc);
    if (e != ESP_OK) return e;
    return rmt_enable(s_chan);
}

static void hw_write(bool on, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t buf[3] = { on ? g : 0, on ? r : 0, on ? b : 0 };   /* GRB */
    rmt_transmit_config_t t = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_enc, buf, sizeof(buf), &t) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, 20);
    }
}

#else  /* mono LED */

static void hw_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_LED_GPIO, !BOARD_LED_ACTIVE_LEVEL);
}

static void hw_write(bool on, uint8_t r, uint8_t g, uint8_t b)
{
    (void)r; (void)g; (void)b;    /* a mono LED ignores colour; rhythm carries the meaning */
    gpio_set_level(BOARD_LED_GPIO, on ? BOARD_LED_ACTIVE_LEVEL : !BOARD_LED_ACTIVE_LEVEL);
}

#endif

/* ------------------------- LED patterns ------------------------- */

/* n=0 means solid on; otherwise n flashes (on/gap) per period, dark for the rest */
typedef struct {
    uint8_t  r, g, b;
    uint8_t  n;
    uint16_t on_ms, gap_ms, period_ms;
} pattern_t;

static pattern_t pattern_of(led_state_t st)
{
    const uint8_t B = LED_BRIGHT_MAX;
    switch (st) {
    /* solid on mono / red on WS2812 -- "needs to be configured" */
    case LED_NO_CFG:     return (pattern_t){ B, 0, 0, 0, 0, 0, 1000 };
    /* one blink every 3 s: idle */
    case LED_IDLE:       return (pattern_t){ B, B / 3, 0, 1, 120, 0, 3000 };
    /* 1 Hz slow blink: cloning -- blue */
    case LED_ADV:        return (pattern_t){ 0, 0, B, 1, 500, 0, 1000 };
    /* triple fast blink: cloning with a client on the UI -- green */
    case LED_ADV_CLIENT: return (pattern_t){ 0, B, 0, 3, 100, 100, 2000 };
    /* double blink: sniffing -- cyan */
    case LED_SNIFF:      return (pattern_t){ 0, B, B / 2, 2, 100, 120, 1600 };
    /* six rapid blinks: error */
    case LED_ERROR:      return (pattern_t){ B, 0, 0, 6, 60, 60, 1200 };
    default:             return (pattern_t){ B, 0, 0, 0, 0, 0, 1000 };
    }
}

static bool pattern_on(const pattern_t *p, uint32_t t)
{
    if (p->n == 0) return true;                       /* solid on */
    uint32_t local = t % p->period_ms;
    uint32_t slot  = (uint32_t)p->on_ms + p->gap_ms;
    for (uint8_t i = 0; i < p->n; i++) {
        uint32_t s = i * slot;
        if (local >= s && local < s + p->on_ms) return true;
    }
    return false;
}

static void led_task(void *arg)
{
    uint32_t t = 0;
    bool last_on = false;
    uint8_t last_r = 0, last_g = 0, last_b = 0;

    for (;;) {
        led_state_t st = s_state;
        pattern_t p = pattern_of(st);
        bool on = pattern_on(&p, t);

        if (on != last_on || p.r != last_r || p.g != last_g || p.b != last_b) {
            last_on = on; last_r = p.r; last_g = p.g; last_b = p.b;
            hw_write(on, p.r, p.g, p.b);
        }

        t += 50;
        if (t >= 60000) t = 0;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void status_led_init(void)
{
#if BOARD_LED_WS2812
    esp_err_t e = ws2812_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ws2812 init failed: %s (check BOARD_LED_GPIO)", esp_err_to_name(e));
    } else {
        hw_write(false, 0, 0, 0);
    }
#else
    hw_init();
#endif
    xTaskCreate(led_task, "led", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "board=%s, led=gpio%d (%s, active=%d)", BOARD_NAME, BOARD_LED_GPIO,
             BOARD_LED_WS2812 ? "ws2812" : "mono", BOARD_LED_ACTIVE_LEVEL);
}

void status_led_set(led_state_t st) { s_state = st; }
