#pragma once

/*
 * 单按键（BOOT/GPIO9）人机交互：
 *   单击  → 监听 ⇄ 复刻 切换（主操作）
 *   双击  → 停止射频（idle）
 *   长按  → 恢复出厂 + 重启（≥5s，物理逃生门）
 *
 * XIAO ESP32-C6 上只有一个可用按键（另一个是 RESET，固件读不到），
 * 所以用"单击/双击/长按"三种手势覆盖全部功能。
 */
typedef void (*btn_single_cb_t)(void);
typedef void (*btn_double_cb_t)(void);
typedef void (*btn_long_cb_t)(void);

void btn_init(btn_single_cb_t on_single, btn_double_cb_t on_double, btn_long_cb_t on_long);
