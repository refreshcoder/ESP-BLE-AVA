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
static btn_long_cb_t   s_long;

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
    bool     long_fired = false;
    uint32_t t_down = 0;
    uint32_t t_last_click = 0;
    uint8_t  clicks = 0;

    for (;;) {
        bool pressed = raw_pressed();
        uint32_t t = now_ms();

        if (pressed && !down) {
            /* 按下：20ms 去抖 */
            vTaskDelay(pdMS_TO_TICKS(20));
            if (raw_pressed()) {
                down = true;
                long_fired = false;
                t_down = now_ms();
            }
        } else if (down && pressed) {
            /* 按住：判长按（只触发一次） */
            if (!long_fired && (t - t_down) >= BOARD_BTN_LONG_MS) {
                long_fired = true;
                clicks = 0;                       /* 长按吃掉之前的连击 */
                ESP_LOGW(TAG, "long press %ums -> factory reset", (unsigned)(t - t_down));
                if (s_long) s_long();
            }
        } else if (down && !pressed) {
            /* 松开：累计一次点击 */
            down = false;
            uint32_t dur = t - t_down;
            if (!long_fired && dur >= BOARD_BTN_SHORT_MS && dur <= BOARD_BTN_SHORT_MAX_MS) {
                if (clicks < 255) clicks++;
                t_last_click = t;
                ESP_LOGD(TAG, "click %u (%ums)", clicks, (unsigned)dur);
            }
        }

        /* 连击窗口结束后分发 */
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

void btn_init(btn_single_cb_t on_single, btn_double_cb_t on_double, btn_long_cb_t on_long)
{
    s_single = on_single;
    s_double = on_double;
    s_long   = on_long;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        /* BOOT 脚板上已有外部上拉；这里开内部上拉做冗余 */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    xTaskCreate(btn_task, "btn", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "button gpio%d: 1click=sniff/clone, 2click=idle, long %dms=factory reset",
             BOARD_BTN_GPIO, BOARD_BTN_LONG_MS);
}
