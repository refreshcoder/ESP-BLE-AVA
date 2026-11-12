#pragma once

/* LED patterns: a mono LED is distinguished by blink rhythm, WS2812 boards also by colour */
typedef enum {
    LED_NO_CFG = 0,     /* solid          : no profile at all */
    LED_IDLE,           /* one blink / 3 s: idle, radio off */
    LED_ADV,            /* 1 Hz blink     : advertising / cloning */
    LED_ADV_CLIENT,     /* triple fast    : cloning and a client is on the UI */
    LED_SNIFF,          /* double blink   : sniffing */
    LED_ERROR,          /* six rapid      : radio error */
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t st);
const char *led_state_str(led_state_t st);
