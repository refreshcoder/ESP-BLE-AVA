# HANDOFF — 换一台机器继续做

> 自包含交接文档。在新主控机上只需要 **本文件 + 仓库**，不需要读历史对话。
> 规范/设计细节以 `README.md`（英文）、`DESIGN.md`（英文设计笔记）、`Phase0.1.MD`（首次上板记录，中文）为准，本文件负责"从零到能改代码"。

- 仓库：<https://github.com/refreshcoder/ESP-BLE-AVA>（public，clone 不需要凭据）
- 交接时 HEAD：`064d1f7`（以 `git log -1` 为准）
- 目标硬件：**ESP32-C6**（默认 Seeed XIAO ESP32-C6）／也支持 **ESP32-C3**
- 固件框架：**ESP-IDF v5.5.6**（≥ v5.2 即可）

---

## 1. 这是什么

单颗 ESP32-C6 上跑的**蓝牙广播复刻器**：**扫到一条 BLE 广播 → 逐字节复刻**。

核心设计约束（不要破坏）：**捕获(SNIFF) 与 广播(CLONE) 互斥运行**，同一时刻只有一个 BLE 角色；所有模式切换必须经过 `main/radio.c`。

```
RMODE_IDLE ──┬──▶ RMODE_SNIFF   (只扫描，把 31B 原始 AD + 扫描响应整段存下来)
             └──▶ RMODE_CLONE   (只广播，ble_gap_adv_set_data() 逐字节重放)
```

管理面：设备自带 SoftAP `BeaconGW-XXXX`（密码 `beaconXXXX`，MAC 后两字节）+ `http://192.168.4.1/` 单页 UI + Bearer token REST API。

---

## 2. 新机器 30 分钟上手

### 2.1 前置

| 项 | 要求 |
|---|---|
| 磁盘 | 建议 **≥ 8 GB 可用**（IDF 源码 ~700 MB + 工具链 ~2.7 GB + 构建 ~200 MB）。装之前可 `pip cache purge` 腾地方 |
| 内存 | 1.5 GB 能编（用 `CMAKE_BUILD_PARALLEL_LEVEL=1`），2 GB 以上更舒服 |
| 系统 | Linux / macOS 最顺；Windows 走 WSL2 或纯 PowerShell（见 §2.4） |

### 2.2 装 ESP-IDF（这里验证过的路径）

```bash
git clone --depth 1 -b v5.5.6 --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf && ./install.sh esp32c6          # C3 也装：./install.sh esp32c6 esp32c3
```

如果系统没有 cmake/ninja，**不用 apt**，直接装进 IDF 的 venv 最省事：

```bash
~/espressif/python_env/*/bin/python -m pip install cmake ninja
```

### 2.3 构建 + 烧写

```bash
git clone https://github.com/refreshcoder/ESP-BLE-AVA && cd ESP-BLE-AVA
. ~/esp-idf/export.sh

idf.py set-target esp32c6      # 或 esp32c3；set-target 会自动选板型预设
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # Ctrl+] 退出
```

**板型随 target 自动选择**：`esp32c6 → BOARD_ID 2 (XIAO C6)`、`esp32c3 → 4 (C3 SuperMini)`；
要换板加 `-DAPP_BOARD=<n>`（预设表见 `main/board.h`）。选错芯片的预设会触发 `#warning`。

### 2.4 只想烧、不想装 IDF

```bash
./tools/flash.sh              # Linux/macOS，自动找端口，烧 build/ 里 4 个文件
./tools/flash.sh /dev/ttyACM0
tools\flash.bat COM7                                   # Windows cmd
.\tools\flash.ps1 -Url https://github.com/refreshcoder/ESP-BLE-AVA/raw/main/firmware/beacon_gw-c6-board2-full.bin
```

`flash.ps1 -Url` 会自己下载合并镜像（单个 bin @ `0x0`），远端 Windows 上最省事。
细节（端口名、RDP 陷阱、MIUI OTG、手机刷写）见 `docs/flashing.md`。

---

## 3. 现状快照

| 项 | 值 |
|---|---|
| 构建 | `idf.py build` esp32c6 / esp32c3 **零警告** |
| 镜像 (C6) | `beacon_gw.bin` ≈ **1.19 MB**，app 槽 1536 KB（余 ~340 KB） |
| 镜像 (C3) | ≈ **0.97 MB**，DRAM 占用 47%（heap 余量 ~166 KB，比 C6 少 100 KB） |
| 静态 RAM (C6) | 186 KB / 452 KB = 41.3% |
| 前端 | `main/www/index.html` 单文件，EN + 简体中文（i18n 表），构建期 gzip 内嵌（~19 KB） |
| CI | GitHub Actions：`esp32c6/board1`、`esp32c6/board2`、`esp32c3/board4`、Frontend（i18n + AD 解析单测）|
| 预编译镜像 | `firmware/beacon_gw-c6-board2-full.bin`（合并镜像，@0x0，可手机直刷；**可能滞后于 main**，见该目录 README） |
| 上板验证 | 已做（见 `Phase0.1.MD`）；**唯一没上板的是"厂商数据解析 + 三种导出"** |

---

## 4. 代码地图

| 文件 | 职责 |
|---|---|
| `main/usb_console.c` | **USB 命令台**（USB-Serial/JTAG，无需网络）：`status/scan/profiles/records/logs/mode/use/clone/reboot` |
| `main/clone_op.c` | 一键克隆的唯一实现，HTTP 与 USB 共用（别在两处各写一份） |
| `main/radio.c` | **RF 模式仲裁（唯一入口）**：`radio_sniff / radio_clone / radio_stop`，内部互斥锁保证"先停旧的再启新的" |
| `main/beacon_scan.c` | 被动+主动扫描、128 条 LRU 探测表、**个人设备噪声过滤**（`scan.filter_noise` 默认开）、**无 AP 客户端时 100% 占空比**（`scan.full_duty`）、**每窗清表**（`scan.clear_each_round`，让 `packets` 成为单窗真值）、窗口时长可配（`scan.duration_s` 默认 120 秒）、存 `raw[31]` 与 `rsp[31]`、间隔 EMA、AD 解析（名字 / iBeacon / **厂商数据 0xFF**） |
| `main/beacon_adv.c` | NimBLE 广播：iBeacon 结构化 or **raw 逐字节重放**；`SCAN_RSP` 重放；地址（random / public boot override）；TX 功率映射 |
| `main/app_cfg.c` | 3 套 profile 的 NVS 单 blob（原子写）、JSON 校验、NVS 计数器（boots/brownout）、LRU 淘汰 |
| `main/storage.c` | `storage` 分区 FATFS+磨损均衡，`/data/cap.bin` = 12B 头 + 128 × 168B 定长记录环形覆盖（raw/rsp/name/mfg/svc/packets/dwell 都存）；点"克隆"写 1 条，扫描窗口结束用 `storage_capture_add_many()` 一趟写完。**挂载失败不再自动格式化**（数据只此一份），需要时用 USB 命令台 `records format`。另有 `/data/pin.bin`（32 槽收藏夹），与归档分离，扫描永不覆盖；每条记录带 `src`（0=克隆 / 1=扫描窗口）。离开 SNIFF 的任何路径（超时 / 三击 / 网页 / 命令台）都经过 `radio_set_sniff_end_cb()` 统一落盘 |
| `main/web_server.c` | 21+ REST 端点、Bearer 鉴权、内嵌 gzip SPA |
| `main/www/index.html` | 单页 UI（5 页签 + 本机记录 + 三种导出），i18n 表 + `data-i18n` |
| `main/board.h` | 板级预设（LED/按键 GPIO），`BOARD_ID` 跟随 target |
| `main/status_led.c` / `btn.c` | 单色灯 6 种闪法 / WS2812 加颜色；单击=切模式、双击=盲扫（60s 窗口，结束整表落盘）、三击=停射频、长按 5s=出厂 |
| `main/net_ap.c` | SoftAP（**只有 AP，没有 STA**）|
| `tools/` | `flash.sh/.bat/.ps1`、`merge.sh`、`ble_watch.py`（Windows 侧判空口）、`check_i18n.js`、`test_mfg.js` |

---

## 5. 五条不变量（改代码前必读）

1. **只有 `radio.c` 能启停射频**。业务层不许直接调 `blescan_start()` / `beacon_adv_apply()`。
2. **禁止逐包写 flash**。捕获表只在 RAM；归档只在"点克隆"时写一条。
3. **UI 文案必须走 i18n 表**（`data-i18n` 属性 / `t()` / `tf()`），`node tools/check_i18n.js` 必须过。
4. **SPA 必须自包含**：无 CDN、无外链字体（设备没有互联网）。
5. **CI 三种构建都要过**：`esp32c6/1`、`esp32c6/2`、`esp32c3/4`。

---

## 6. 已验证 vs 未验证

**已上板验证**（`Phase0.1.MD` 详细记录，含空口 vs 串口对照）：刷机、RAW 逐字节克隆、公开地址克隆、扫描响应复制、模式持久化。

**上板中发现并已修的缺陷**（**不要回退**）：

| 编号 | 症状 | 根因 |
|---|---|---|
| D1 | TX 功率设置永远失败 | dBm 与 `esp_power_level_t` 映射错（C6 是扩展档位表） |
| D2 | 广播停了但状态不更新 | `s_mode` 不跟随实际射频状态 |
| D3 | "断 AP 后自动回 sniff"、莫名复位 | **主机侧硬关闭串口句柄 → 板子复位**，不是固件问题 |
| D4 | `tx_dbm` 与实际不符 | 上报的是请求值而非生效值 |
| D5 | 克隆的地址反了 | NimBLE 地址是小端，工程其余部分是大端 |
| D6 | 灯全反（看起来像死机） | **XIAO C6 的灯是低电平点亮** |
| D7 | 单击按键 panic | `btn` 任务栈只有 2560 B → 栈溢出 |
| D8 | 捕获归档永远为空（`storage` 分区 832 KB 全 `0xFF`）、`/api/records` 恒 `available:false` | **`storage_init()` 全工程没有调用点**：`s_ready` 恒 false → `storage_capture_add()` 直接返回 `ESP_ERR_INVALID_STATE`。已在 `app_main()` 补上 |

**编译期才暴露的三个坑**（同样勿回退）：

1. `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n` 时 **`ble_svc_gap_*` 根本不链接** → 本项目不初始化任何 GAP/GATT 服务。
2. **`scan_start` 与 `libnet80211.a(ieee80211_scan.o)` 符号冲突**（只有开 WiFi 才暴露）→ 扫描侧统一前缀 `blescan_*`。
3. `CONFIG_LOG_DEFAULT_LEVEL_INFO` 必须写 `=y`；`CONFIG_BT_CTRL_MODEM_SLEEP` **只在 C3 存在**（C6 用 `CONFIG_PM_ENABLE`）。

**仍未上板**：厂商数据解析（`mfg_company/mfg_vendor/mfg_src`）+ JSON/CSV/raw 三种导出 + 前端 AD 解析回填。
上板只需确认一件事：目标设备的厂商数据是否落进 `mfg`、`mfg_src` 是 `adv` 还是 `scan_rsp`。

---

## 7. 已知限制

1. **>31 字节的扩展广播无法复刻**（legacy adv 上限）。
2. **主动扫描要花空口**：observer 每条广播发一个 `SCAN_REQ`；克隆只在源 profile 可扫描时才回 `SCAN_RSP`。
3. **公开地址只能每次开机设置一次**（`esp_iface_mac_addr_set()`），切到/切离这类 profile 会**重启一次**，期间轮换暂停。
4. **没有 OTA**，升级只能 USB。
5. 单射频分时：官方 coexist 表把 `SoftAP connected × BLE` 标为 `C1`（性能不稳定），C3/C5/C6 **一模一样**，换芯片解决不了，只能靠"模式互斥"规避。

---

## 8. 待办（按优先级）

| # | 任务 | 预估 | 验收标准 |
|---|---|---|---|
| 1 | 上板验证厂商数据解析 + 导出 | 30 min | `/api/scan` 出现 `mfg_company/mfg_vendor`；CSV/raw 内容对得上；`mfg_src` 合理 |
| 2 | **STA + HTTP OTA + 回滚** | ~250 行 | 设备能连现场 Wi-Fi（保留 SoftAP 兜底）；`POST /api/ota` 上传 `beacon_gw.bin` 后重启进新槽；新固件 30 s 内未确认健康自动回滚。**这是"板子在远端"场景的唯一解** |
| 3 | 「导出当前实时表」（区别于浏览器累积记录） | ~30 行 | 另一个浏览器/设备上抓的记录也能导出 |
| 4 | 天线切换（XIAO：`GPIO3` 拉低启用 RF 开关，`GPIO14` 低=内置陶瓷 / 高=u.FL） | ~40 行 | `POST /api/antenna {"external":true}` 持久化并在开机生效 |
| 5 | 包率偏低排查（Phase0.1 未解项：25 s 只收到 4 包） | 需上板 | 与源设备配置间隔吻合 |

OTA 的技术底座已就绪：分区表 `ota_0/ota_1` 各 1536 KB + `otadata` 8 KB；只差把 `app_update` 加进 `main/CMakeLists.txt` 的 `REQUIRES` 并写 `esp_ota_*` 流程。回滚开关在 `components/bootloader/Kconfig.app_rollback`（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）。

---

## 9. 踩坑清单（省你半天）

| 场景 | 坑 | 处理 |
|---|---|---|
| 远端 Windows + 单 Wi-Fi 网卡 | 连板子 SoftAP 会**掐断自己的 RDP** | 用 `tools/usb_cli.py` 走 USB 命令台，完全不碰网络 |
| 远端 Windows + 无 Python | `py`/`python` 只是 Store 的 stub，跑起来什么都不输出 | `winget install Python.Python.3.12 --scope user`（user scope，不需要管理员），再 `python -m pip install esptool`；esptool v5 子命令是 `read-flash` 不是 `read_flash` |
| 远端刷机 | RDP **注销**会杀掉刷写进程（断开没事） | 只断开；或 `schtasks` 起任务 |
| Linux 刷机 | **ModemManager 抢串口**导致刷一半失败 | `sudo systemctl stop ModemManager` |
| 刷完看日志 | **硬 kill 串口进程会复位板子**（D3） | `Ctrl+]` 正常退出 |
| 手机 OTG 刷机 | 米 UI 的"OTG 10 分钟后自动关闭"会在中途断电 | 设置里改成不自动关闭 |
| iPhone (Lightning) | 无 USB Host 供电、也无串口 API，**刷写不可能** | 只当"充电宝供电 + 网页管理" |
| 供电 | **CR2032 带不动**（内阻 10~40 Ω，峰值 350~420 mA）；但**刷写时只有 50~150 mA**，手机 OTG 够用 | 运行要 USB/充电宝/3×AA/CR123A |
| 天线 | XIAO 默认走内置陶瓷天线，**插 u.FL 不生效** | 见待办 4 |
| 配置丢失 | 长按 5 s 会恢复出厂（两段式，`57153fe` 之后改过） | 别误按 |

---

## 10. 常用命令速查

```bash
. ~/esp-idf/export.sh

idf.py set-target esp32c6 && idf.py build            # 构建
idf.py -p /dev/ttyACM0 flash monitor                 # 烧 + 看日志（Ctrl+] 退出）
idf.py size                                          # 体积/RAM

./tools/merge.sh                                     # 4 文件 → 单个 @0x0 镜像
./tools/flash.sh /dev/ttyACM0                        # 刷 build/
node tools/check_i18n.js                             # UI 文案键完整性
node tools/test_mfg.js                               # 前端 AD 解析单测（15 例）

# USB 命令台（无需网络；需要 pip install pyserial）
python tools/usb_cli.py -p COM7 status
python tools/usb_cli.py -p COM7 scan
python tools/usb_cli.py -p COM7 mode sniff
python tools/usb_cli.py -p COM7 clone a4c1382a5122

# 空口判定（Windows，需要 pip install bleak）
python tools/ble_watch.py -s 20 --expect <固件 raw hex>

# 常用 API（带 token）
curl -H "Authorization: Bearer $T" http://192.168.4.1/api/status
curl -H "Authorization: Bearer $T" "http://192.168.4.1/api/scan?limit=16"
curl -H "Authorization: Bearer $T" -X POST http://192.168.4.1/api/clone -d '{"addr":"aabbccddeeff"}'
curl -H "Authorization: Bearer $T" -X POST http://192.168.4.1/api/mode  -d '{"mode":"sniff"}'
```

---

## 11. 交接注意

1. **推送凭据**：仓库是 public，clone/构建不需要任何凭据；只有 `git push` 需要（当前用的是仓库 Deploy key，在别的机器上要换成你自己的 PAT/SSH key）。
2. **`firmware/*.bin` 会进 git**（每次重建 ~1.3 MB）。这是为了"没有 PC 也能手机刷"。长期建议改成 GitHub Release 发布，避免仓库膨胀（需要 `repo` 权限的 token 建 release）。
3. **CI 免费额度**：一次 push 跑 4 个 job（3 次 IDF 构建，每个 ~3-4 分钟）。别频繁空推。
4. 改完代码请按 §5 的 5 条不变量自查，再 `push`；CI 会替你挡住符号冲突、i18n 漏键、两套板型编译不过这三类问题。
