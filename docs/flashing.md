# Flashing

Two ways to get firmware onto the board. Path A needs no toolchain (5 minutes);
path B is for when you want to change the code.

---

## A. Flash a prebuilt image (no ESP-IDF install)

You need four files: `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`,
`beacon_gw.bin`. Get them from the CI artifact of any green build:

> GitHub → **Actions** → latest successful `build` run → **Artifacts** →
> `firmware-esp32c6-board2.zip` (XIAO C6) or `firmware-esp32c3-board4.zip` (C3 SuperMini).

Unzip it — you get a `build/` directory holding exactly those four files.

```bash
pip install esptool          # one time

# Linux / macOS
./tools/flash.sh                      # auto-detects the port
./tools/flash.sh /dev/ttyACM0         # or name it

# Windows
tools\flash.bat COM7
```

The script is a thin wrapper around:

```bash
esptool.py --chip esp32c6 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xf000  build/ota_data_initial.bin \
  0x20000 build/beacon_gw.bin
```

### Which port is the board?

| OS | Port |
|---|---|
| Linux | `/dev/ttyACM0` (native USB). `/dev/ttyUSB0` means the board has a CH340 bridge instead |
| macOS | `/dev/cu.usbmodem*` |
| Windows | a `COMx` that appears when you plug in — no driver needed (CDC) |

`esptool` detects the ESP32-C6 USB-Serial/JTAG peripheral (`303a:1001`) and performs the
DTR/RTS reset itself, so **you do not need to hold BOOT**. If the port never appears or
`Failed to connect` shows up, hold **BOOT** while plugging the cable in, then retry.

---

## B. Build and flash from source

```bash
. $IDF_PATH/export.sh                 # ESP-IDF >= v5.2 (v5.5.6 verified)
idf.py set-target esp32c6             # or esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor  # Ctrl+] to exit the monitor
```

`set-target` also picks the default board preset (`esp32c6` → XIAO C6,
`esp32c3` → C3 SuperMini). Override with `-DAPP_BOARD=<n>`; the presets are listed in
`main/board.h`.

---

## First boot: what you should see

1. **Serial** (115200 on the same USB port):
   ```
   I (312) app: board=XIAO ESP32-C6 reset=power_on boots=1 brownouts=0
   I (410) led: board=XIAO ESP32-C6, led=gpio15 (mono, active=1)
   I (980) ap: SSID='BeaconGW-3F2A' auth=3
   I (1020) radio: mode=sniff (scan win=60ms/300ms)
   I (2100) app: ready: 1 profile(s), active=0, ap='BeaconGW-3F2A'
   ```
2. **LED** — double blink = sniffing (the default mode). Steady on = no profile.
   Dark? Flip `BOARD_LED_ACTIVE_LEVEL` in `main/board.h` and rebuild.
3. **Wi-Fi** — join `BeaconGW-XXXX`, password `beaconXXXX` (the XXXX is the last two
   bytes of the MAC; the exact SSID/password are in the serial log).
4. **UI** — open <http://192.168.4.1/>, sign in with the *SoftAP password*, then
   **Capture** tab → tap **Clone** on a nearby beacon.

If the LED works but the page does not: check `/api/status` via serial-less means only
after joining the AP — a wrong password, or a scan duty above 50 %, are the usual causes.

---

## Verify the image is really running our firmware

```bash
curl -H "Authorization: Bearer <token>" http://192.168.4.1/api/status
```

Expect `"board": "XIAO ESP32-C6"`, `"ver": "1.0.0"`, and a `reset_reason` that is not
`brownout`.

---

## Recovering a board that no longer boots

The four files above include the bootloader and partition table, so a full re-flash
restores everything — a bad *application* image cannot brick the chip. If the USB port
does not enumerate at all, hold **BOOT** while plugging in to force the ROM downloader.

There is no OTA endpoint in this version, so updating always means a cable.

---

## Flashing from an Android phone

Possible, and it works fine for this chip because the **ESP32-C6 USB-Serial/JTAG
peripheral (`303a:1001`) speaks the same SLIP protocol as esptool over a cable** — no
CP210x/CH340 driver is involved.

Power is *not* the problem people expect: in ROM download mode the radios are off, so
current stays around 50–150 mA. The 350–420 mA figure only applies when the firmware is
running with the SoftAP up.

### One file instead of four (recommended for phones)

```bash
./tools/merge.sh      # -> build/beacon_gw-full.bin  (≈1.3 MB)
```

Then every phone tool only needs **one file at offset `0x0`**, which removes most of the
ways this can go wrong.

### Route A — Chrome + Web Serial (no install)

Web Serial reached Chrome for Android in **148** (before that it was Bluetooth-RFCOMM
only). Check `chrome://version` first; MIUI's built-in browser will not work.

1. Open <https://espressif.github.io/esptool-js/> in Chrome
2. **Connect** → pick the device (a permission dialog appears) → baud `460800`
3. **Add file**: `beacon_gw-full.bin` at offset `0x0`
   (or the four files at `0x0`, `0x8000`, `0xf000`, `0x20000`)
4. **Program** → wait, then the board reboots

### Route B — Termux + nrflash (no root, scriptable)

```bash
# Termux from F-Droid (not the Play Store build) + Termux:API from F-Droid
pkg update && pkg install python termux-api libusb
pip install nrflash

nrflash probe                                   # chip auto-detected
nrflash write 0x0:beacon_gw-full.bin            # single merged image
# or: nrflash write 0x0:bootloader.bin 0x8000:partition-table.bin \
#                   0xf000:ota_data_initial.bin 0x20000:beacon_gw.bin
```

`nrflash` talks to the USB endpoints directly (Android hands apps a file descriptor via
`termux-usb`, not a `/dev/ttyACM*` node), uploads the ESP-IDF RAM stub, and prompts for
USB permission once per session. Native-USB chips such as C3/C6 do not need baud
negotiation.

### Route C — third-party APKs

There are Android apps that claim C6 support (for example `Dzarda7/esp-flasher-android`
or `AvinasheeTech/ESP32-phone-flasher`). They work, but they are unmaintained to varying
degrees; prefer A or B.

### Fastest route with no PC at all

A merged image for the default preset is committed to the repo, so a phone can fetch it
with one tap:

```
https://github.com/refreshcoder/ESP-BLE-AVA/raw/main/firmware/beacon_gw-c6-board2-full.bin
```

Flash it at offset **`0x0`** — nothing else to configure. See
[`firmware/README.md`](../firmware/README.md) for the commit it was built from.

### Where do the files come from without a PC?

Download the CI artifact in the phone browser (GitHub → Actions → green run → Artifacts),
unzip it, and you get `build/` with the four images. A merged image has to be produced
once with `tools/merge.sh` (needs a machine), after which the single `.bin` can live
wherever you like.

### MIUI / HyperOS gotchas

* **Settings → Additional settings → OTG connection** may be set to "turn off after
  10 minutes". Set it to stay on, or the cable can go dead mid-flash.
* Choose "File transfer" if MIUI asks what the USB connection is for; a charging-only
  selection can leave the data lines unpowered on some builds.
* Keep the cable short and do not let the screen lock/sleep during Programming.

### Still not recommended for routine updates

Flashing over a phone cable is fine as a rescue path, but HTTP OTA over the device's own
SoftAP remains the better field-update route (not implemented yet — see DESIGN.md §13).
