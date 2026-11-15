#include "btn.h"
#include "board.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "btn";

static btn_single_cb_t s_single;
static btn_double_cb_t s_double;
static btn_hold_cb_t   s_hold;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline bool raw_pressed(void)
{
    return gpio_get_level(BOARD_BTN_GPIO) == BOARD_BTN_ACTIVE_LEVEL;
}

static void btn_task(void *arg)
{
    bool     down = false;
    bool     warn_fired = false;
    bool     long_fired = false;
    uint32_t t_down = 0;
    uint32_t t_last_click = 0;
    uint8_t  clicks = 0;

    for (;;) {
        bool pressed = raw_pressed();
        uint32_t t = now_ms();

        if (pressed && !down) {
            /* pressed: 20 ms debounce */
            vTaskDelay(pdMS_TO_TICKS(20));
            if (raw_pressed()) {
                down = true;
                warn_fired = false;
                long_fired = false;
                t_down = now_ms();
            }
        } else if (down && pressed) {
            uint32_t held = t - t_down;
            /* Two-stage hold: 5 s only warns (LED + log), 8 s commits. Letting go in between
             * is the cancel path -- the whole point is that a fumbled press on a button 6 mm
             * from RESET must not be able to wipe the configuration. */
            if (!warn_fired && held >= BOARD_BTN_LONG_MS) {
                warn_fired = true;
                clicks = 0;                       /* a hold swallows pending clicks */
                ESP_LOGW(TAG, "held %ums -> factory reset at %ums, release to cancel",
                         (unsigned)held, (unsigned)BOARD_BTN_COMMIT_MS);
                if (s_hold) s_hold(BTN_HOLD_WARN);
            }
            if (warn_fired && !long_fired && held >= BOARD_BTN_COMMIT_MS) {
                long_fired = true;
                ESP_LOGW(TAG, "held %ums -> factory reset", (unsigned)held);
                if (s_hold) s_hold(BTN_HOLD_COMMIT);
            }
        } else if (down && !pressed) {
            /* released: cancel a warned hold, otherwise count a click */
            down = false;
            uint32_t dur = t - t_down;
            if (warn_fired && !long_fired) {
                warn_fired = false;
                ESP_LOGI(TAG, "released after %ums -> cancelled", (unsigned)dur);
                if (s_hold) s_hold(BTN_HOLD_CANCEL);
            } else if (!long_fired && dur >= BOARD_BTN_SHORT_MS && dur <= BOARD_BTN_SHORT_MAX_MS) {
                if (clicks < 255) clicks++;
                t_last_click = t;
                ESP_LOGD(TAG, "click %u (%ums)", clicks, (unsigned)dur);
            }
        }

        /* dispatch once the multi-click window expires */
        if (!down && clicks > 0 && (t - t_last_click) >= BOARD_BTN_MULTI_MS) {
            if (clicks == 1) {
                ESP_LOGI(TAG, "single click");
                if (s_single) s_single();
            } else {
                ESP_LOGI(TAG, "double click (%u)", clicks);
                if (s_double) s_double();
            }
            clicks = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void btn_init(btn_single_cb_t on_single, btn_double_cb_t on_double, btn_hold_cb_t on_hold)
{
    s_single = on_single;
    s_double = on_double;
    s_hold   = on_hold;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        /* The BOOT pin already has an external pull-up; internal pull-up is belt and braces */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    xTaskCreate(btn_task, "btn", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "button gpio%d: 1click=clone on/off, 2click=radio off, hold %dms=warn/%dms=factory reset",
             BOARD_BTN_GPIO, BOARD_BTN_LONG_MS, BOARD_BTN_COMMIT_MS);
}
