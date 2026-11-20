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
| Built from commit | `4c29b37` (branch `main`, CI run 44690856868) |
| `-app.bin` SHA-256 | `289edd4c57f93d1f39af1fb25b2800b48df791e3ec85b9f4e9e81e19e6c380b3` |
| Source size | 1,387,584 bytes |
| SHA-256 | `de65e43ae26221f5d2fad49be10afbefab8b89a4d211f3d343ea6d31c4ab7d8b` |

After flashing, the board raises `BeaconGW-XXXX` (password `beaconXXXX`) and the UI is at
<http://192.168.4.1/>. The LED double-blinks while sniffing, which is the boot default.

> Rebuilt by hand at the commit above. The image **can lag `main`** — the previous one
> predated the bench-run fixes (LED polarity, button stack overflow, address byte order),
> so re-check the commit and SHA-256 before reporting a bug.
