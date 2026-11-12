## What changed

<!-- one paragraph; link an issue if there is one -->

## Type

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / cleanup
- [ ] Documentation only
- [ ] Build / CI only

## Verification

- [ ] `idf.py -DAPP_BOARD=1 build` passes
- [ ] `idf.py -DAPP_BOARD=2 build` passes
- [ ] No new compiler warnings
- [ ] Tested on hardware (say which board and how it was powered)

## Checklist

- [ ] Nothing outside `radio.c` calls `blescan_start()` / `beacon_adv_apply()` directly
- [ ] No new per-packet writes to NVS or the filesystem
- [ ] Any change to `main/www/index.html` is still a single self-contained file
      (no CDN, no external fonts — the device has no internet)
- [ ] If `board.h` defaults changed, `docs/hardware.md` was updated too
