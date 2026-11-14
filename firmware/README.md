# Prebuilt images

**Convenience builds — not the source of truth.** Prefer the CI artifacts or your own
`idf.py build` when you can; these exist so a phone with no PC can flash in one tap.

| File | Target | Board preset | Flash at |
|---|---|---|---|
| `beacon_gw-c6-board2-full.bin` | esp32c6 | `2` = Seeed XIAO ESP32-C6 | `0x0` |

The merged image contains bootloader + partition table + otadata + application, produced
by [`tools/merge.sh`](../tools/merge.sh):

```bash
idf.py set-target esp32c6 && idf.py build && ./tools/merge.sh
```

Flash it with `esptool.py write_flash 0x0 <file>` or any Web Serial flasher
(see [docs/flashing.md](../docs/flashing.md)).

| | |
|---|---|
| Built from commit | `3be5751` |
| Source size | 1,302,640 bytes |
| SHA-256 | `c8991afd6073ed865c3abc9590502faf5f3757dca537196e354b77b06aa88a86` |

After flashing, the board raises `BeaconGW-XXXX` (password `beaconXXXX`) and the UI is at
<http://192.168.4.1/>. The LED double-blinks while sniffing, which is the boot default.

> These files are rebuilt by hand when the format or the flashing path changes, so they
> can lag `main`. Check the commit above before reporting a bug.
