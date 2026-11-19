# 把「钉钉打卡点」写进板子（新电脑操作指南）

> 目标：在一台**全新的电脑 C** 上，把已经抓到的钉钉打卡点（M1 Pro）广播数据写进这块
> XIAO ESP32-C6，让板子开始广播它。
>
> 全程**不需要装任何软件**（Windows 10/11 自带 `curl.exe`），**不需要 ESP-IDF**，
> **不需要把板子拿到打卡点旁边**（因为我们已经有抓到的原始数据）。
>
> 需要的两样东西：板子 + 一根能传数据的 USB 线（只供电用充电宝也行，Wi-Fi 那条路要用它上电）。

---

## 0. 手上的数据（这段 31 字节就是打卡点）

```
02011617ff0001b5000209641da700000090c21065ab011000000003033cfe
│      │ │     │                                        │
│      │ │     └ 厂商载荷 20 字节（b5 00 02 开头 = 钉钉）  └ 服务 0xFE3C（固件里显示 DingTalk）
│      │ └ 公司 ID 0x0100（SIG 名 TomTom，实际是钉钉）
│      └ AD 类型 0xFF = Manufacturer Data
└ Flags（LE 通用可发现 + 不支持经典蓝牙 + 同时支持）
```

**这条数据是「静态版」**（载荷尾数 `00 00 00`，不是动态加密的 `xx 00 0d/0b/0f`），
所以克隆一次长期有效。

---

## 路 A：Wi-Fi + curl（推荐，零安装，5 分钟）

板子会自己开一个热点，把手机/电脑连上去就能直接下命令。

### A1. 连上板子的热点

| 项 | 值 |
|---|---|
| SSID | `BeaconGW-XXXX` |
| 密码 | `beaconxxxx`（**同一组两位十六进制，小写**） |
| 板子地址 | `http://192.168.4.1` |

`XXXX` / `xxxx` 由板子的 SoftAP MAC 后两字节决定，出厂就是这样配的：

- 在电脑 C 的 Wi-Fi 列表里找 **`BeaconGW-` 开头**的那个（只有一个板子的话就那一个）；
- 如果板子被改过 SSID，接 USB 用串口看开机日志里的 `ap: ssid=...`。

> ⚠️ 连上之后电脑 C **没有外网**，这是正常的（板子的热点不带联网）。

### A2. 换一个访问令牌

```powershell
curl.exe -s -X POST http://192.168.4.1/api/session -H "Content-Type: application/json" -d "{\"password\":\"beaconxxxx\"}"
```

返回：

```json
{"ok":true,"token":"<32位十六进制>"}
```

把返回的 32 位 token 记下来（每次重新生成都不同），**后面两条命令都要用**。

> 密码里如果不止这两位，就把 `beaconxxxx` 换成你实际的 AP 密码；AP 密码和 token 都能通过。

### A3. 写入 profile（这就是"把打卡点写进板子"）

```powershell
curl.exe -s -X POST http://192.168.4.1/api/profiles -H "Authorization: Bearer <TOKEN>" -H "Content-Type: application/json" -d "{\"name\":\"DingTalk M1Pro\",\"kind\":1,\"raw\":\"02011617ff0001b5000209641da700000090c21065ab011000000003033cfe\",\"adv_itvl_ms\":100}"
```

返回：

```json
{"ok":true,"id":2}
```

`kind:1` = 原始字节模式（`kind:0` 是 iBeacon 参数模式，这里不能用）。
`id` 是这套 profile 的编号（板子上已有 2 套，所以新的很可能就是 2）。

### A4. 让它开始广播

```powershell
curl.exe -s -X POST http://192.168.4.1/api/mode -H "Authorization: Bearer <TOKEN>" -H "Content-Type: application/json" -d "{\"mode\":\"clone\"}"
```

返回 `{"ok":true,"mode":"clone"}`。

**更简单的办法**：拔掉电脑，直接**单击板子上的 BOOT 键**（0.45 秒内不要连点两次），
LED 变成 **1 Hz 慢闪** 就表示正在广播。

---

## 路 B：USB 串口 + `clone <addr>`（设备必须在范围内）

如果手头没有原始数据、只有板子，那就只能站到打卡点旁边抓：

1. 串口工具连上板子：**115200 / 8N1 / 无流控**（PuTTY 或 Python 都行），端口在设备管理器里看 `COMx`。
2. 敲命令（每行回车）：

```
status                  # 确认固件在跑，看到 "mode":"idle"
clear                   # 清空扫描表
mode sniff              # 开始扫描，等 5~10 秒
records                 # 看有没有出现打卡点：svc = "0xFE3C"、svc_name = "DingTalk"
clone faa89ac1ab65      # ← 换成 records 里看到的地址
```

3. 成功返回：

```json
{"ok":true,"id":1,"adv_itvl_ms":82,"replaced":false,"mode":"clone"}
```

失败会返回 `not in capture table, run a scan first` —— 意思是表里还没有它，**再靠近、再多等几秒**重试。

> 这条路要求：**板子在打卡点 1 m 内**，且 `records` 里已经出现它。
> 距离超过 3~5 m 基本抓不到（那台机器发射功率很低）。

---

## 验证：板子真的在广播那份数据吗？

三条互相独立，任一条通过即可：

**① LED**

| LED | 含义 |
|---|---|
| **1 Hz 慢闪** | ✅ 正在广播 |
| 每 3 秒闪一下 | 空闲（射频关闭） |
| 一直常亮 | 空闲，且一个配置都没有 |
| **六连快闪** | ❌ 失败（按下单击后立刻出现 = 没有可广播的数据） |

**② 接口自查**（路 A 的 token 还有效时）

```powershell
# 当前模式
curl.exe -s -H "Authorization: Bearer <TOKEN>" http://192.168.4.1/api/status
# 列表里应该有 name=DingTalk M1Pro、raw=02011617... 的那条，且 active=true
curl.exe -s -H "Authorization: Bearer <TOKEN>" http://192.168.4.1/api/profiles
```

**③ 用第二台手机的 nRF Connect 看**

打开 nRF Connect → 扫描 → 找到板子（MAC 不是打卡点的 MAC，这没关系，钉钉不校验 MAC）
→ 展开 → `RAW` 应该**逐字节等于**：

```
0x02011617FF0001B5000209641DA700000090C21065AB01100000000303CFE
```

> 手机**收不到自己发的广播** —— 必须用**另一台**设备（另一部手机 / 蓝牙耳机配对列表里出现都算佐证）。

---

## 断电之后怎么办

profile 存在板子的 flash 里（NVS），**断电不丢**。重新上电后板子默认是空闲状态，
**单击一次 BOOT** 就恢复广播（或再发一次 A4 那条 `mode:clone`）。

---

## 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `curl.exe` 提示不是内部命令 | 用的是 PowerShell 的 `curl` 别名 → 必须写 **`curl.exe`** |
| `401 Unauthorized` | token 不对或过期 → 重新做 A2；`Authorization: Bearer ` 后面**有空格** |
| `400 profile: raw must be 2..31 bytes hex` | raw 多了/少了字节（必须正好 31 字节 = 62 个十六进制字符） |
| `409 Conflict` | profile 满了 → 先删一条：`curl.exe -X DELETE .../api/profiles/<id>` |
| `404` / 连不上 192.168.4.1 | 电脑连的不是板子的热点，或板子没上电 |
| 加完 profile 但 LED 不是慢闪 | 忘了 A4（`mode:clone`）或单击 BOOT |
| 六连快闪 | profile 里没有有效载荷 → 检查 `kind:1` 和 `raw` |
| 板子上电就进下载模式、不广播 | 上电时**不要按住 BOOT**；按一下 RESET |

---

## 附：如果还要重烧固件（可选）

只有换新板子 / 固件损坏时才需要。Windows 上一条命令装 esptool：

```powershell
py -m pip install esptool
```

下载 → 校验 → 写 → 回读比对（**别跳过校验**）：

```powershell
curl.exe -sSL -o app.bin "https://cdn.jsdelivr.net/gh/refreshcoder/ESP-BLE-AVA@535dbde/firmware/beacon_gw-c6-board2-app.bin"
certutil -hashfile app.bin SHA256
# 必须等于 629e4cd6adbde65ffc733cf0ad24bf4627b400ed64088255f7554b7b87d42e0e

py -m esptool --chip esp32c6 -p COM7 -b 460800 write-flash 0x20000 app.bin
py -m esptool --chip esp32c6 -p COM7 -b 460800 read-flash  0x20000 0x132DB0 verify.bin
certutil -hashfile verify.bin SHA256     # 必须和上面一模一样
```

> 只烧 `0x20000`（app 分区），**不碰 bootloader / 分区表 / NVS** —— 配置和已存的
> profile 都不会丢。镜像大小 `1256880` = `0x132DB0`。

---

## 一页速查

```
连 Wi-Fi   BeaconGW-XXXX / beaconxxxx → http://192.168.4.1
取 token   POST /api/session {"password":"beaconxxxx"}
写 profile POST /api/profiles {"name":"DingTalk M1Pro","kind":1,"raw":"02011617ff0001b5...03033cfe","adv_itvl_ms":100}
开始广播   POST /api/mode {"mode":"clone"}   或 单击 BOOT
验证       LED 1 Hz 慢闪 / GET /api/profiles / 第二台手机看 RAW 逐字节相同
断电重启   单击 BOOT 恢复广播
```