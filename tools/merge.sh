#!/usr/bin/env sh
# Merge the 4 image files into a single flashable image at offset 0x0.
#
#   ./tools/merge.sh                 # -> build/beacon_gw-full.bin
#
# A single binary is handy for phone-based flashers (esptool-js / ESP Launchpad /
# nrflash) that are easier to point at one file + offset 0x0 than at four files.
#
# Requires: pip install esptool
set -eu
BINDIR="${BINDIR:-build}"
CHIP="${CHIP:-esp32c6}"
OUT="${OUT:-${BINDIR}/beacon_gw-full.bin}"

ESPTOOL="$(command -v esptool.py || command -v esptool)"

"$ESPTOOL" --chip "$CHIP" merge_bin -o "$OUT" \
    --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0     "${BINDIR}/bootloader/bootloader.bin" \
    0x8000  "${BINDIR}/partition_table/partition-table.bin" \
    0xf000  "${BINDIR}/ota_data_initial.bin" \
    0x20000 "${BINDIR}/beacon_gw.bin"

ls -l "$OUT"
echo "flash this single file at offset 0x0"
