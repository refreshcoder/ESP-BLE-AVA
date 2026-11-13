# Hardware notes

Everything here is about the **ESP32-C6 / ESP32-C3** and the five supported boards. The
firmware uses exactly **two GPIOs** (LED + button), so any board of either chip works
after editing `main/board.h`.

## Supported boards

| `APP_BOARD` | Board | On-board LED | BOOT button | Flash |
|---|---|---|---|---|
| `1` | Espressif **ESP32-C6-DevKitC-1** | WS2812 RGB on **GPIO8** | **GPIO9** | 4 MB (N4/N8 variants exist) |
| `2` (default on C6) | Seeed **XIAO ESP32-C6** | single-colour LED on **GPIO15** | **GPIO9** | 4 MB |
| `3` | Waveshare **ESP32-C6-Zero** | verify silkscreen | **GPIO9** | 4 MB or 8 MB depending on SKU |
| `4` (default on C3) | **ESP32-C3 SuperMini** | single-colour LED on **GPIO8**, usually active low | **GPIO9** | 4 MB |
| `5` | Espressif **ESP32-C3-DevKitM-1** | WS2812 RGB on **GPIO8** | **GPIO9** | 4 MB |

`BOARD_ID` defaults from the build target (`esp32c6` → 2, `esp32c3` → 4), so
`idf.py set-target esp32c3 && idf.py build` just works. A `#warning` fires if a preset
obviously belongs to the other chip, which usually means a stale `-DAPP_BOARD`.

```bash
idf.py -DAPP_BOARD=2 build      # switch at build time, no source edit needed
```

## Pin usage

| Function | Pin | Notes |
|---|---|---|
| Status LED | `BOARD_LED_GPIO` (`8` or `15`) | WS2812 is driven over RMT; a mono LED is driven as a plain GPIO |
| Button | **GPIO9** | BOOT pin, active low. On the XIAO the other button is RESET and cannot be read by firmware |
| Native USB | **C6: GPIO12 (D-), GPIO13 (D+)**<br>**C3: GPIO18 (D-), GPIO19 (D+)** | The most common cross-wiring mistake between the two chips |
| UART0 (fallback console) | GPIO21 (TX), GPIO20 (RX) | usually not brought out; only needed if USB-JTAG misbehaves |

The XIAO silk prints `D0…D10`; map those to GPIO numbers from Seeed's pinout diagram
rather than from memory — the C6 variant differs from the C3 one.

Strapping pins on the C6 include GPIO8/GPIO9/GPIO15. On a custom board copy the
official devkit's pull resistors and do not hang a load on a strapping pin that could
change the power-on level.

## Which LED do I have?

* **WS2812 / RGB (DevKitC-1)** — set `BOARD_LED_WS2812 1`; colour *and* blink rhythm
  are used.
* **Single colour (XIAO, most Zero clones)** — set `BOARD_LED_WS2812 0`; only the blink
  rhythm carries information.

If the LED never lights, flip `BOARD_LED_ACTIVE_LEVEL` between `1` and `0`. If colours
look wrong on a WS2812 board, the data pin is not `BOARD_LED_GPIO`.

## Power

Average current with the SoftAP up plus advertising at 100 ms is **110–130 mA**;
peaks reach **350–420 mA** during coexistence. That peak is what breaks weak supplies.

| Supply | Works? | Notes |
|---|---|---|
| USB port of a computer | ✅ | Use a short, thick cable |
| 5 V charger / power bank | ✅ | Most reliable for bench and field work |
| Phone OTG | ⚠️ | Many phones current-limit OTG to 100–500 mA and cut VBUS when the screen sleeps |
| iPhone (Lightning) | ❌ | Needs the MFi camera adapter, which cannot supply the peak and offers no usable serial API |
| 3×AA (4.5 V) into 5V pin | ✅ | ~7.5 Wh, runs through the on-board LDO |
| CR123A (3 V) into 3V3 pin | ✅ | 1 A pulse capability, small footprint |
| **CR2032 coin cell** | ❌ | 10–40 Ω internal resistance; the TX peak collapses the rail and causes brownout resets |

Symptoms of an inadequate supply: `brownouts` in `/api/status` climbing,
`Brownout detector was triggered` on the console, or silent reboot loops when
advertising starts. Mitigations: shorter/thicker USB cable, powered hub, and
**220–470 µF across 3V3/GND** to absorb the transmit bursts.

Do **not** connect a non-rechargeable cell to the XIAO `BAT+/BAT-` pads — that input
feeds the Li-ion charger.

## Antenna

* The DevKitC-1 and XIAO use a PCB antenna plus a u.FL connector; the XIAO is much
  smaller, so its antenna efficiency is lower (expect a few dB less).
* Keep **15 mm of clear space** around the antenna end, away from metal, batteries and
  screws. A metal enclosure or mounting against a metal post costs 6–10 dB.
* For anything in an enclosure, use the u.FL/IPEX variant and an FPC antenna.
* Never copy a calibrated `ref_rssi` value between boards — calibrate each unit at 1 m.

## Console

The console is routed to the **native USB-Serial/JTAG**, so it appears as
`/dev/ttyACM0` (Linux), `/dev/cu.usbmodem*` (macOS) or a COM port (Windows) — no
driver, no CH340. Boards that do have a bridge chip enumerate as `/dev/ttyUSB0`; in that
case switch the console back to UART in `menuconfig`.

Flashing normally does not need the BOOT button. If the port disappears, hold BOOT while
plugging in to force the ROM downloader.

## Nothing on the serial console?

Every diagnostic that matters is available without a cable:

```
GET /api/status   ->  reset_reason, boots, brownouts, mode, tx_dbm,
                      adv_starts, scan_pkts, storage, heap / min_heap
```

Join the SoftAP, open <http://192.168.4.1/>, and read the **Status** tab. This is the
recommended path on phones that cannot do USB serial.
