# 机器 C 上把打卡点写进板子（纯 USB，不用 Wi-Fi）

> 目标：在一台**全新电脑 C** 上，只用 USB 线把钉钉打卡点（M1 Pro）的广播数据写进
> XIAO ESP32-C6，并让它开始广播。
>
> 需要的只有两样工具：**Python + esptool**（烧写）+ 一个串口终端（Python 十行脚本即可，
> 不想装 PuTTY 也行）。**不需要 ESP-IDF，不需要连板子热点，不需要靠近打卡点。**

---

## 0. 一句话流程

```
装 esptool → 校验 sha256 → 烧 0x20000 → 串口敲 adv <hex> → LED 慢闪 = 完成
```

---

## 1. 准备

| 东西 | 说明 |
|---|---|
| 板子 | XIAO ESP32-C6（MAC 形如 `10:bd:a3:…`） |
| 数据线 | **必须能传数据**（纯充电线只会供电，电脑看不到串口） |
| 固件文件 | `beacon_gw-c6-board2-app.bin`，sha256 见 §3 |
| Python | 3.8+（Windows 上 `py -V` 能出版本号就行） |

固件怎么拿到（二选一）：

- **从本机拷**：`D:\repo\ESP-BLE-AVA\firmware\beacon_gw-c6-board2-app.bin`（已经是新版本，可直接 U 盘带走）；
- **机器 C 有网就下**：

```powershell
curl.exe -sSL -o app.bin "https://cdn.jsdelivr.net/gh/refreshcoder/ESP-BLE-AVA@a58344c/firmware/beacon_gw-c6-board2-app.bin"
```

---

## 2. 装 esptool

```powershell
py -m pip install -U esptool
py -m esptool version
```

期望看到 `esptool v5.x` 或更高。

> 机器 C 没网：在有网的机器上 `py -m pip download esptool -d wheels`，
> 把 `wheels\` 整个目录拷过去，再 `py -m pip install --no-index -f wheels esptool`。

---

## 3. 校验固件（必做）

```powershell
certutil -hashfile app.bin SHA256
```

必须等于：

```
c395c91ba2aa7ac0750aec653b2ec48aebf2d24f0403599268d9c46e8c5de137
```

大小 **1257776** 字节（`0x133130`）。**对不上就别烧**，重新拷/重新下。

---

## 4. 找到串口号

插上板子，然后：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

或者看设备管理器 → **端口 (COM 和 LPT)** → `USB 串行设备 (COMx)`。
也可以：

```powershell
Get-PnpDevice -PresentOnly | ? InstanceId -match 'VID_303A' | % FriendlyName
```

看到 `USB JTAG/serial debug unit` 就说明板子认到了。
下面所有命令把 `COM7` 换成你实际的号。

> 板子没出现？换能传数据的线 → 换 USB 口（别用前置口/HUB）→ 按住 BOOT 再插。

---

## 5. 烧写（只烧 app，配置不丢）

```powershell
# 写
py -m esptool --chip esp32c6 -p COM7 -b 460800 write-flash 0x20000 app.bin

# 回读比对（别跳过这一步）
py -m esptool --chip esp32c6 -p COM7 -b 460800 read-flash 0x20000 0x133130 verify.bin
certutil -hashfile verify.bin SHA256
```

第二次的 sha256 必须和 §3 **一模一样**。写的时候会打印
`Wrote 1257776 bytes ... Hash of data verified.`，那就是成功。

> - 只写 `0x20000`（app 分区），**不碰 bootloader / 分区表 / NVS** → 已有配置和 profile 都在。
> - **如果这块板子从来没烧过我们的固件**（全新板 / 别的固件），先烧整包：
>   `beacon_gw-c6-board2-full.bin` 写到 `0x0`（sha256 `b7671a8e68d0cfa782b33913075928f4b4c67b3a8d5e700789db6f12b61bae62`，大小 1388848），
>   然后再按上面烧一次 app.bin。**整包会清掉配置。**

---

## 6. 连上串口

### 方式 A（推荐）：十行 Python，敲一条命令看结果

存成 `send.py`，和 `app.bin` 放同一个目录：

```python
import serial, sys, time
p = sys.argv[1]                       # COM7
s = serial.Serial(p, 115200, timeout=0.3)
s.dtr = False; s.rts = False
time.sleep(5)                         # 板子上电后会重启一次，等它稳定
for cmd in sys.argv[2:]:
    s.reset_input_buffer(); s.write((cmd + "\r\n").encode())
    print(">>>", cmd)
    end = time.time() + 40
    while time.time() < end:
        line = s.readline().decode("utf-8", "replace").rstrip()
        if line == "#END": break
        if line: print("   ", line)
s.close()
```

用法（先确认固件在跑）：

```powershell
py -m pip install pyserial
py send.py COM7 status
```

能看到一行 `{"ok":true,...,"mode":"idle",...}` 就说明通了。

### 方式 B：PuTTY

`Connection type: Serial`、`Serial line: COM7`、`Speed: 115200`、`8-N-1`、`Flow control: None`，
并且在 **Terminal → Local echo 设为 Force on**（否则你敲的字自己看不见）。

---

## 7. 写入打卡点并开始广播

**这一条命令就是全部**：

```powershell
py send.py COM7 adv 02011617ff0001b5000209641da700000090c21065ab011000000003033cfe
```

期望输出：

```json
{"ok":true,"id":2,"raw_len":31,"adv_itvl_ms":100,"replaced":false,"mode":"clone"}
```

含义：`raw_len` 31 字节、广播间隔 100 ms、`mode` 已经切到 `clone`（正在广播）。

这条 31 字节就是打卡点：

```
02011617ff0001b5000209641da700000090c21065ab011000000003033cfe
│      │ │     │                                         │
│      │ │     └ 厂商载荷 20 字节（b5 00 02 开头 = 钉钉）  └ 服务 0xFE3C（固件显示 DingTalk）
│      │ └ 公司 ID 0x0100（SIG 里叫 TomTom，实际是钉钉）
│      └ 0xFF = Manufacturer Data
└ Flags
```

> 这条数据属于**静态版**（尾数 `00 00 00`，不是动态加密的 `xx 00 0d/0b/0f`），
> 所以抄一次长期有效，不需要实时中继。

---

## 8. 验证

```powershell
py send.py COM7 status profiles
```

- `status` 里应为 `"mode":"clone"`、`"adv":true`；
- `profiles` 里应有一条 `"name":"usb-raw"`、`"kind":1`、`"raw_len":31`、`"active":true`，
  且 `"raw"` 就是上面那串。

**LED**：**1 Hz 慢闪**（亮 0.5 s / 灭 0.5 s）= 正在广播。
每 3 秒闪一下 = 空闲；一直常亮 = 没配置；**六连快闪** = 失败。

**第二台手机**（必须有，手机收不到自己发的）：装 nRF Connect → 扫描 → 找到板子
（MAC 和打卡点不一样，无妨，钉钉不校验 MAC）→ 展开 → `RAW` 应逐字节等于：

```
0x02011617FF0001B5000209641DA700000090C21065AB01100000000303CFE
```

---

## 9. 断电以后

profile 存在 flash 里，**断电不丢**。重新上电后板子是空闲状态，
**单击一次 BOOT** 就恢复广播（`BOOT` 键，单击，0.45 秒内别连点）。

想换 payload：再敲一次 `adv <新的 hex>` —— 会**原地覆盖**同一条 `usb-raw`，
不会把配置塞满。

---

## 10. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| 找不到 COM 口 | 线是纯充电线 / 换 USB 口 / 按住 BOOT 再插 |
| `A fatal error occurred: Could not open COM7` | PuTTY 或 `send.py` 还占着串口 → 先关掉再烧 |
| 烧完 sha256 不一致 | 别用，重烧；连续不一致换线换口 |
| `adv` 返回 `unknown command` | 板子里还是**旧固件**（没有 adv 命令）→ 回 §5 烧 `app.bin` |
| `adv` 返回 `raw must be 2..31 bytes hex` | hex 长度不对：必须 **62 个字符**（31 字节），不能有空格/换行 |
| `adv` 返回 `config full ...delete one first` | profile 满了 → `py send.py COM7 profiles` 找一个删，或先 `factory yes`（会清配置） |
| 有 profile 但 LED 不是慢闪 | 没生效 → 再敲 `mode clone`，或单击 BOOT |
| LED 六连快闪 | 没有可广播的载荷 → 重做 §7 |
| 手机搜到了但 RAW 不一样 | 你手机上看到的是别的设备；按 MAC/名字确认是板子 |
| 板子一上电就进不了应用 | 上电时别按住 BOOT；按一下 RESET |

---

## 11. 备用方案（都不需要，知道有就行）

- **设备在 1 m 内**：老固件也能用的抓取式克隆
  `clone faa89ac1ab65`（地址从 `records` 里看），效果一样。
- **不想用串口**：板子会开热点 `BeaconGW-XXXX`（密码 `beaconXXXX`），
  连上去 `POST http://192.168.4.1/api/profiles`，body
  `{"name":"DingTalk","kind":1,"raw":"02011617...3cfe"}`，另需先
  `POST /api/session {"password":"<AP密码>"}` 取 token 放 `Authorization: Bearer`。

---

## 一页速查

```
找串口   [System.IO.Ports.SerialPort]::GetPortNames()
校验     certutil -hashfile app.bin SHA256   → c395c91b…e137
烧写     py -m esptool --chip esp32c6 -p COM7 -b 460800 write-flash 0x20000 app.bin
回读     py -m esptool ... read-flash 0x20000 0x133130 verify.bin  → 再校验一次
写入     py send.py COM7 adv 02011617ff0001b5000209641da700000090c21065ab011000000003033cfe
验证     status → mode=clone / profiles → usb-raw 31B / LED 1Hz 慢闪 / 第二台手机看 RAW
断电后   单击 BOOT
```

> ⚠️ 状态说明：`adv` 命令是这一版新增的（commit `991049b`），CI 编译通过，但**还没在真机上跑过一次**。
> 如果它返回 `unknown command`，说明板子里是旧固件（按 §5 烧完就有）。