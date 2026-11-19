# Prebuilt images

**Convenience builds — not the source of truth.** Prefer the CI artifacts or your own
`idf.py build` when you can; these exist so a phone with no PC can flash in one tap.

| File | Target | Board preset | Flash at |
|---|---|---|---|
| `beacon_gw-c6-board2-full.bin` | esp32c6 | `2` = Seeed XIAO ESP32-C6 | `0x0` |
| `beacon_gw-c6-board2-app.bin` | esp32c6 | `2` = Seeed XIAO ESP32-C6 | `0x20000` |

`-app.bin` is the **application only**: it does not touch the bootloader, the partition table
or **NVS**, so the stored profiles survive the update. Use it for an app-only update over
serial (`esptool.py write_flash 0x20000 <file>`); use the merged `-full.bin` when the board
must come up from a blank flash (at the cost of erasing the config).

The merged image contains bootloader + partition table + otadata + application, produced
by [`tools/merge.sh`](../tools/merge.sh):

```bash
idf.py set-target esp32c6 && idf.py build && ./tools/merge.sh
```

Flash it with `esptool.py write_flash 0x0 <file>` or any Web Serial flasher
(see [docs/flashing.md](../docs/flashing.md)).

| | |
|---|---|
| Built from commit | `17eda51` (branch `feat/usb-console`, CI run 44588600456) |
| `-app.bin` SHA-256 | `c395c91ba2aa7ac0750aec653b2ec48aebf2d24f0403599268d9c46e8c5de137` |
| Source size | 1,388,848 bytes |
| SHA-256 | `b7671a8e68d0cfa782b33913075928f4b4c67b3a8d5e700789db6f12b61bae62` |

After flashing, the board raises `BeaconGW-XXXX` (password `beaconXXXX`) and the UI is at
<http://192.168.4.1/>. The LED double-blinks while sniffing, which is the boot default.

> Rebuilt by hand at the commit above. The image **can lag `main`** — the previous one
> predated the bench-run fixes (LED polarity, button stack overflow, address byte order),
> so re-check the commit and SHA-256 before reporting a bug.
