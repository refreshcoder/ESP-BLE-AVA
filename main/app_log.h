#pragma once
#include <stdint.h>
#include <stdbool.h>

#define APP_VER      "1.0.0"
#define LOG_RING_SZ  256
#define LOG_MSG_MAX  56

enum { LOG_T_BOOT = 0, LOG_T_CFG, LOG_T_ACT, LOG_T_DETECT, LOG_T_SCAN, LOG_T_ERR };

typedef struct {
    uint32_t seq;
    uint32_t up_ms;
    uint8_t  type;
    int8_t   rssi;
    char     msg[LOG_MSG_MAX];
} logrec_t;

void     applog_init(void);
void     applog_add(uint8_t type, int8_t rssi, const char *fmt, ...);
uint32_t applog_last_seq(void);
/* 返回写入条数; emit 里不要阻塞 */
int      applog_snapshot(uint32_t since_seq, int max, void (*emit)(const logrec_t *, void *), void *ctx);
void     applog_clear(void);
const char *applog_type_str(uint8_t t);
