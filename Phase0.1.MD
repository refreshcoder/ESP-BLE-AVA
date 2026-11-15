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
| 2b | 广播地址 | ⚠️ 当时实测**未复制**（源为公开地址）。现已实现 `clone_public`（boot 时设置，需重启），见"后续改动" |
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

## 后续改动（本次全新增加，**未编译、未上板**）

### SCAN_RSP 复制

之前的结论表里"Scan Response 完全没有"是当时的事实。现已实现，改动如下：

| 文件 | 改动 |
|---|---|
| `beacon_scan.c` | `params.passive = 0`（主动扫描）；`event_type == 0x04` 的第二个事件写入 `rsp[31]` 并**提前返回**（不污染 `raw`、间隔 EMA、包计数） |
| `beacon_scan.h` | `detection_t` 新增 `rsp[31]` + `rsp_len` |
| `app_cfg.h/.c` | profile 新增 `rsp[31]` + `rsp_len`，JSON 里为可选字段 `rsp` |
| `beacon_adv.c` | 每次 apply 都调 `ble_gap_adv_rsp_set_data()`（空时传 NULL/0 清除，避免上一个 profile 的响应泄漏）；仅在 `scannable` 时生效 |
| `web_server.c` | `/api/scan` 输出 `rsp`/`rsp_len`；`/api/clone` 复制 rsp，并把"抓到过扫描响应"视为源可扫描 |
| `index.html` | 捕获行多一个"扫描响应"十六进制折叠块；高级编辑显示 rsp；保存时保留 rsp |

### 公开地址克隆（`clone_public`，切换配置自动重启）

**已验证（受控实验）**：UI 里把广播地址指定为 `00:01:22:33:44:55` 后，空口最强的那条
（−46 dBm，midea payload）地址确实等于该值 → `esp_iface_mac_addr_set(ESP_MAC_BT, …)` 在 C6 上生效。

**模式持久化已验证**：点击克隆后，用 esptool 触发一次重启（未按任何键），重启后空口仍然是
−46 dBm、地址 `00:01:22:33:44:55` 的克隆广播 → `cfg.last_mode` 恢复生效。

同时发现并修复两个问题：

1. **重启后总是回到 sniff**（设计如此，但地址切换必须重启 → 克隆会静默停掉）：模式现在持久化在
   `cfg.last_mode`，重启后恢复上次模式；只有首次启动/恢复出厂后才是 sniff。
2. **SPA 被浏览器缓存**：`Cache-Control: max-age=3600` 导致刷完固件仍加载旧 UI（缺字段），
   已改为 `no-cache`。

之前结论里"公开地址物理上不可复制"已被乐鑫文档推翻：`esp_iface_mac_addr_set(ESP_MAC_BT, …)`
可在**不烧 eFuse** 的前提下在 RAM 里改 BLE public 地址，代价是必须在任何网络接口初始化前
调用 → **每次启动只能一个地址**。

| 文件 | 改动 |
|---|---|
| `main.c` | `app_main` 在 `net_ap_start()` 之前，若活动 profile 带 `clone_public_addr` 则 `esp_iface_mac_addr_set(ESP_MAC_BT, src_addr)`，然后用 `esp_read_mac` 记下本次启动的地址 (`cfg_note_boot_addr`) |
| `app_cfg.*` | profile 新增 `clone_public_addr`（JSON 可选字段）；`cfg_addr_change_pending()` 比较活动 profile 想要的地址与本次启动已应用的地址 |
| `radio.c` | `radio_clone()` 成功后若 `cfg_addr_change_pending()` 为真 → 1.2 s 后重启（留时间把 HTTP 响应发出） |
| `main.c` tick | 任何 profile 带 `clone_public_addr` 时暂停 rotation（否则每步都要重启），并告警一次 |
| `web_server.c` | `h_clone` 在源地址为公开类型时置 `clone_public_addr = true` |
| `index.html` | 配置卡新增"公开地址"标签 + 一句说明 |

与之前实测的差异：Phase 0.1 里观测到的克隆体地址与原件不同（`24:59:E5:B6:E6:1A` vs
`10:BD:A3:B4:2A:52`），是因为当时固件没有这个能力。新固件上两者应**完全一致**且 `TxAdd = 0`。

待验证：

1. `esp_iface_mac_addr_set()` 在 IDF v5.5 存在（CI 编过即证明）
2. 设置后 `ble_hs_util_ensure_addr()` 确实走 public（串口 `own_addr_type=0` 那一行）
3. 空口上 `TxAdd = 0` 且地址等于 `src_addr`（需 nRF Connect 或 WinRT 原始节）

### 已刷入 CI 构建的固件（提交 45616fa）

CI 成功后用已存储的 git 凭据拉取 artifact `firmware-esp32c6-board2`（下载接口需鉴权，未鉴权返回 401），
按文档偏移刷入：

| 偏移 | 文件 | 字节 |
|---|---|---|
| `0x0` | `bootloader.bin` | 22,064 |
| `0x8000` | `partition-table.bin` | 3,072 |
| `0xf000` | `ota_data_initial.bin` | 8,192 |
| `0x20000` | `beacon_gw.bin` | 1,183,328（sha256 `dba6600d…85aab2`） |

四次写入均 `Hash of data verified`。NVS（`0x9000`）未擦除，原有 2 个 profile 保留。

刷后确认：`esptool --before no-reset` 报 `Write timeout` → ROM 不应答，**应用已在运行**；
CI size report：app `0x120e60` = 1,183,328 B，1.5 MB 分区还剩 **25%**；DIRAM 187,654 / 452,112 B = **41.5%**。

注意：`midea2` 这个 profile 是旧固件建的，JSON 里没有 `clone_public_addr`（默认 false），
所以本次启动**不会**冒用空调的地址。要验证公开地址克隆，需用新固件重新克隆一次源设备。

待验证（需要有工具链的机器）：

1. `ble_gap_adv_rsp_set_data(NULL, 0)` 在本 IDF/NimBLE 版本确实能清除旧响应（若不行，改成每次显式写入 1 字节空数据）
2. 主动扫描下 `event_type` 确实为 `0x04`（常量已用数字，避免头文件差异）
3. 包率/功耗变化：主动扫描每个广播多发一个 SCAN_REQ

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
