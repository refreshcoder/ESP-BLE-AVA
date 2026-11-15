# Phase 0.1 — 首次上板验证记录

日期：2026-11-15
对象：Seeed XIAO ESP32-C6（板载 USB-Serial/JTAG，`303A:1001`，COM10）
固件：`firmware/beacon_gw-c6-board2-full.bin`（app ver `3be5751`，ESP-IDF v5.5.6）

验证方式：**不依赖手机**。本机 Realtek BT 5.0 适配器 + `tools/ble_watch.py`（Windows
WinRT/bleak 被动/主动扫描）抓空口，同一时间用 pyserial 抓 COM10 串口做对照。

---

## 结论

| # | 项目 | 结果 |
|---|---|---|
| 1 | 刷机（合并镜像 @0x0） | ✅ 校验通过，启动正常 |
| 2 | 克隆功能（RAW 逐字节重放） | ✅ 空口 payload 与固件 raw 完全一致，两次独立复现 |
| 3 | "断开 AP 后自动回到 sniff" | ❌ 不存在该逻辑；实际是主机强制关闭串口导致复位（见 D3） |
| 4 | TX 功率设置 | ❌ 每次 `ESP_FAIL`（单位/枚举用错），实际功率为控制器默认值 |
| 5 | 广播"自己消失" | ⚠️ 观察到两次；`s_mode` 不跟随，UI 会显示"克隆中 + ADV off" |
| 6 | 空口包率 | ⚠️ 25 s 收到 4 包（配置 1725 ms 应有 ~14），见"未解" |

---

## 证据 A — 克隆成功（空口 vs 串口）

串口（`clone on:` 打印的是真正送进 `ble_gap_adv_set_data()` 的字节）：

```
I (630487) adv: clone on: 'midea2' raw=02010606096d6964656112ffa806013531313031373353453330303331
                     itvl=1725ms tx=9dBm (scannable)
I (630488) radio: mode=clone (ESP_OK)
```

空口（手机已断开 SoftAP 的状态下，25 s 主动扫描）：

```
10:BD:A3:B4:2A:52  -48dB  4pkts  midea | a806013531313031373353453330303331
                  MATCH  name 06096d69646561, mfg 12ffa806013531313031373353453330303331
```

逐段核对：

| AD 段 | 固件 raw | 空口实测 | |
|---|---|---|---|
| flags `0x01` | `02 01 06` | bleak 不暴露（假定值） | ⚠️ |
| Complete Local Name `0x09` | `06 09 6d 69 64 65 61` = "midea" | 一致 | ✅ |
| Manufacturer Data `0xFF` | `12 ff a8 06 01 51 31 … 33 31`（公司 ID 0x06A8 + 17 B） | 一致 | ✅ |
| 身份 | 板子 +9 dBm 贴着电脑 | −48 dBm（真空调在 −86…−96 dBm） | ✅ 是板子 |

> 注意：真空调的 manufacturer data 会**轮换状态字节**（同一条 `24:59:E5:B6:E6:1A`
> 先后广播过 `…3531313031…` 和 `…5fcc321ae6b6e5592401`），所以"payload 相同"只能作为
> 克隆体存在的证据，不能反过来当原件的身份标识；克隆体是**冻结副本**，payload 不变。

## 证据 B — 为什么看起来"断 AP 就回 sniff"

- 代码里唯一能改射频模式的地方：BOOT 按键、HTTP `/api/mode|active|clone|scan`、
  rotation tick、`radio_on_ble_ready()`（**仅在 BLE 协议栈 sync 时调用 = 启动/复位后**）。
- `main/net_ap.c:36` 的 `WIFI_EVENT_AP_STADISCONNECTED` 只做 `s_sta--` 和打日志。
- 判定依据：按键**之前** LED 是双闪（=sniff）。既然没有任何断连路径，只能是复位。
- 旁证：串口 uptime 从 2096 s 掉到 288 s，说明这期间确实重启过（真因见 D3）。

→ **假象定案："断连导致回 sniff" 不成立，真因是复位后 `radio_on_ble_ready()` 默认 sniff。**

---

## 缺陷清单

### D1（已修）TX 功率：把 dBm 当枚举传

`main/beacon_adv.c:snap_tx_power()` 原代码：

```c
esp_err_t e = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, (esp_power_level_t)v);
```

`esp_power_level_t` 是**索引**不是 dBm：`N12=0, N9=1, N6=2, N3=3, N0=4, P3=5, P6=6, P9=7`。
传 `9` 越界（C6 上 9 = `ESP_PWR_LVL_N11`）→ 每次都 `ESP_FAIL`，射频停在默认功率，而
`/api/status` 的 `tx_dbm` 却报 9 dBm。

修法：`idx = dBm/3 + 4`，并 clamp 到 0…7（−12…+9 dBm）。失败时不更新 `s_tx_dbm`。

### D2（已修）广播掉了但状态不跟着变

`radio.c:79` 用 `beacon_adv_is_running()` 只在**调用 `radio_clone()` 的那一刻**同步
`s_mode`。之后若 controller 停止广播（`BLE_GAP_EVENT_ADV_COMPLETE` 或射频错误），
`s_running=false` 而 `s_mode` 仍是 `RMODE_CLONE` → UI 长期显示"克隆中 / ADV off"，
系统也不会自己恢复。

修法：`main.c` 的 1 s tick 加自愈——`radio_mode()==RMODE_CLONE && !beacon_adv_is_running()`
时每 10 s 重新 apply 一次并写日志（与 rotation 复用同一条 `app_apply_active()` 路径）。

### D3（已定案，非固件问题）复位原因 = 主机侧强制关闭串口

`reset: unknown` + `brownout` 计数从未写入。三次受控实验（同一台 PC，COM10）：

| # | 动作 | 结果 |
|---|---|---|
| 1 | 正常 `Serial()` 打开 → 静置 6 s | 不复位 |
| 2 | 同一句柄上 `dtr=False; rts=False` | **复位**，`rst:0x15 (USB_UART_HPSYS)` |
| 3 | `close()` 后立刻重新打开 | 不复位 |

`rst:0x15` 是 USB-Serial/JTAG 外设发出的芯片复位，ESP-IDF 的 `esp_reset_reason()` 把它映射成
`ESP_RST_UNKNOWN` —— 与界面上看到的 `reset: unknown` 一致。

触发源头是**抓包进程被 `Stop-Process -Force` 杀掉**：句柄被硬断，USB CDC 拆除时 DTR/RTS
落到电平，芯片随之复位。时间线与 `boots` 完全对得上（每次 kill + 重启抓包，uptime 归零）：

- 2096 s → 288 s（12:5x）↔ `killed 34888 / killed 34004`
- 覆盖到 "ready: 2 profile(s)" 的那次（13:10）↔ `killed 26580` + 重启抓包

行为符合设计：复位后 `radio_on_ble_ready()` 会把模式拉回 sniff，所以现象是"没人动它就从
克隆回到双闪"。`boots=9` 里还包括 esptool write/read-flash 的 3 次 hard reset。

**处置**：抓串口时不要 kill 抓包进程，让它按 `--seconds` 自然超时退出（实验 3 已证明优雅
关闭不会复位）；需要中断就先记下这次会重启板子。固件无需改动。

**副作用（已踩）**：`dtr=0/rts=0` 同时把 GPIO9 拉低，芯片重启后会停在 **ROM 下载模式**
——应用不跑，表现为 LED 全黑、无广播、AP 也起不来。判据：`esptool --before no-reset`
能直接接上 bootloader（应用在跑时必然 `Write timeout`）。恢复：

```powershell
python -m esptool --chip esp32c6 -p COM10 --before no-reset --after hard-reset read-mac
# 复查：这条应报 Write timeout，说明 ROM 不再应答、应用已启动
python -m esptool --chip esp32c6 -p COM10 --before no-reset --after no-reset read-mac
```

→ 不要手动摆弄 DTR/RTS；要重启就用 `--after hard-reset` 或按 RESET 键。

### D4（已修）`tx_dbm` 与实际不符

随 D1 一并解决：`s_tx_dbm` 现在只记录**设置成功后**的实际档位。

---

## 未解 / 待验

1. **空口包率**：25 s 只有 4 包。干扰因素有两个，尚未分离：
   - Windows 侧扫描占空比本身很低（工具侧限制）；
   - SoftAP 仍在广播，官方共存评级 SoftAP×BLE = `C1`「支持但不稳定」（DESIGN.md §6）。
   目前固件**没有关闭 SoftAP 的接口**，要分离变量需要先加开关。
2. `flags` 段（`02 01 06`）本机工具看不到，`MATCH` 是按段匹配的；完整 31 字节仍需
   nRF Connect 的 RAW 视图或 WinRT 直接读 `DataSections`。

---

## 复现命令

```powershell
# 1) 串口对照（后台起一个进程，让它自己到点退出；**不要 kill**，硬断句柄会复位板子，见 D3）
python $env:TEMP\bgw_serial_cap.py COM10 tools\ser.log 900

# 2) 空口判定：--expect 用串口 clone on 打印的 raw
python tools\ble_watch.py -s 25 --active --min-rssi -70 --expect 02010606096d6964656112ffa806013531313031373353453330303331

# 3) 周边全景
python tools\ble_watch.py -s 15 --active
```

实测要点（踩过的坑）：

- 这块 Realtek 在 **passive** 模式几乎收不到板子的包（板子发的是 `ADV_SCAN_IND`），
  必须 `--active`。
- 板子插在同一台电脑上，`--min-rssi -70` 就能把远处同名空调全部滤掉。
- `--expect` 的匹配是按 AD 段做的，不依赖 flags 与段顺序。

## 本机 vs 手机（能力边界）

| 判据 | 本机 `ble_watch.py` | nRF Connect |
|---|---|---|
| 名称 / 厂商数据 / Service Data 段逐字节 | ✅ | ✅ |
| 源地址、RSSI、包数、间隔 | ✅ | ✅ |
| 完整 31 字节（含 flags、AD 顺序） | ❌（只能按段重建比较） | ✅ RAW |
| 地址类型 Public/Random、ADV_IND/ADV_SCAN_IND、可连接性 | ❌ | ✅ |
