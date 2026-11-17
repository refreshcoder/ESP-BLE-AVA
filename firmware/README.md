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
| Built from commit | `8efce56` (branch `feat/usb-console`, CI run 44072214982) |
| `-app.bin` SHA-256 | `174bf45ec25fbd36f13d6a4bf8ed7d9b9e9c716dca619afa3aa3ab1d2eb82fd9` |
| Source size | 1,317,488 bytes |
| SHA-256 | `4b97992c2d99ad73b0613aa502a03b80867a71adbad549448f1ee7d1ee48dca1` |

After flashing, the board raises `BeaconGW-XXXX` (password `beaconXXXX`) and the UI is at
<http://192.168.4.1/>. The LED double-blinks while sniffing, which is the boot default.

> Rebuilt by hand at the commit above. The image **can lag `main`** — the previous one
> predated the bench-run fixes (LED polarity, button stack overflow, address byte order),
> so re-check the commit and SHA-256 before reporting a bug.
