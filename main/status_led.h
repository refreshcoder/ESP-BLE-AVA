#pragma once

/* 灯语：单色灯用"闪法"区分，WS2812 板额外用颜色区分（见 status_led.c） */
typedef enum {
    LED_NO_CFG = 0,     /* 常亮        ：没有任何配置 */
    LED_IDLE,           /* 每 3s 一闪  ：模式 idle（射频停） */
    LED_ADV,            /* 1Hz 慢闪    ：复刻/广播中 */
    LED_ADV_CLIENT,     /* 三连快闪    ：复刻中 + 有人连着管理页 */
    LED_SNIFF,          /* 双闪        ：监听捕获中 */
    LED_ERROR,          /* 6 连急闪    ：射频错误 */
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t st);
const char *led_state_str(led_state_t st);
