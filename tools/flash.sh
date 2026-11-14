#!/usr/bin/env sh
# Flash the 4 image files produced by a build (or by the CI artifact) with esptool.
#
#   ./tools/flash.sh                          # build/  + auto-detect port, esp32c6
#   ./tools/flash.sh /dev/ttyACM0             # explicit port (Linux/macOS)
#   CHIP=esp32c3 ./tools/flash.sh COM7        # C3 target, explicit Windows port
#   BINDIR=~/Downloads/build ./tools/flash.sh # tree unpacked from the CI artifact
#
# Requires: pip install esptool
set -eu

BINDIR="${BINDIR:-build}"
PORT="${1:-${ESPPORT:-}}"
CHIP="${CHIP:-esp32c6}"

BOOT="${BINDIR}/bootloader/bootloader.bin"
PT="${BINDIR}/partition_table/partition-table.bin"
OTADATA="${BINDIR}/ota_data_initial.bin"
APP="${BINDIR}/beacon_gw.bin"

missing=0
for f in "$BOOT" "$PT" "$OTADATA" "$APP"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; missing=1; }
done
[ "$missing" -eq 0 ] || { echo "run 'idf.py build' first, or set BINDIR to the unpacked artifact" >&2; exit 1; }

command -v esptool.py >/dev/null 2>&1 || command -v esptool >/dev/null 2>&1 || {
    echo "esptool not found: pip install esptool" >&2; exit 1; }

ESPTOOL="$(command -v esptool.py || command -v esptool)"

PORT_ARG=""
[ -n "$PORT" ] && PORT_ARG="-p $PORT"

echo "chip=$CHIP  bindir=$BINDIR  port=${PORT:-auto}"
# -b 460800 is safe on USB-JTAG; drop to 115200 if the link is unhappy.
"$ESPTOOL" --chip "$CHIP" $PORT_ARG -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0     "$BOOT" \
    0x8000  "$PT" \
    0xf000  "$OTADATA" \
    0x20000 "$APP"

echo
echo "done. The board now boots into the firmware:"
echo "  SoftAP  BeaconGW-XXXX   (password beaconXXXX, last 2 MAC bytes)"
echo "  UI      http://192.168.4.1/   (log in with the SoftAP password)"
