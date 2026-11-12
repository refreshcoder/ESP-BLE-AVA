#include "app_log.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static logrec_t          s_ring[LOG_RING_SZ];
static int               s_head;      /* next write position */
static int               s_count;
static uint32_t          s_seq;
static SemaphoreHandle_t s_mtx;

static const char *k_type[] = { "boot", "cfg", "act", "detect", "scan", "err" };

const char *applog_type_str(uint8_t t) { return t < sizeof(k_type) / sizeof(k_type[0]) ? k_type[t] : "?"; }

void applog_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_head = s_count = 0;
    s_seq  = 0;
}

void applog_add(uint8_t type, int8_t rssi, const char *fmt, ...)
{
    if (!s_mtx) return;
    /* Format on the stack, then only copy inside the critical section */
    char msg[LOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    logrec_t *r = &s_ring[s_head];
    r->seq   = ++s_seq;
    r->up_ms = (uint32_t)(esp_timer_get_time() / 1000);
    r->type  = type;
    r->rssi  = rssi;
    memcpy(r->msg, msg, sizeof(r->msg));
    s_head = (s_head + 1) % LOG_RING_SZ;
    if (s_count < LOG_RING_SZ) s_count++;
    xSemaphoreGive(s_mtx);
}

uint32_t applog_last_seq(void)
{
    uint32_t v;
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    v = s_seq;
    xSemaphoreGive(s_mtx);
    return v;
}

int applog_snapshot(uint32_t since_seq, int max, void (*emit)(const logrec_t *, void *), void *ctx)
{
    if (!s_mtx || !emit) return 0;
    int n = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int start = (s_head - s_count + LOG_RING_SZ) % LOG_RING_SZ;
    for (int i = 0; i < s_count && n < max; i++) {
        const logrec_t *r = &s_ring[(start + i) % LOG_RING_SZ];
        if (r->seq <= since_seq) continue;
        emit(r, ctx);
        n++;
    }
    xSemaphoreGive(s_mtx);
    return n;
}

void applog_clear(void)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_head = s_count = 0;
    xSemaphoreGive(s_mtx);
}
