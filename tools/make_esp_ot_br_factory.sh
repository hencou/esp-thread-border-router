#!/usr/bin/env bash
# Build a single merged flash image for ESPConnect / web-based flashing.
#
# Usage:
#   ./tools/make_esp_ot_br_factory.sh [output.bin]
#   WIN_DOWNLOADS=/mnt/c/Users/<name>/Downloads ./tools/make_esp_ot_br_factory.sh
#                                         # also copies the merged bin to your Windows Downloads
#
# Prereqs:
#   - ESP-IDF env loaded (`. ~/esp/esp-idf/export.sh`)
#   - the project has been configured for its target (`idf.py set-target esp32s3`)

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
OUT="${1:-${PROJECT_DIR}/esp_ot_br_factory.bin}"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py not on PATH — source ESP-IDF first: . ~/esp/esp-idf/export.sh" >&2
    exit 1
fi

cd "${PROJECT_DIR}"

# merge-bin depends on the build target, so an out-of-date or missing build/ is rebuilt here.
idf.py build

if [[ ! -f "${BUILD_DIR}/flash_args" ]]; then
    echo "ERROR: build/flash_args is missing after the build" >&2
    exit 1
fi

# idf.py merge-bin reads build/flash_args and automatically picks up the
# same files and offsets 'idf.py flash' would use (bootloader,
# partition table, otadata, app image, and any extra flash targets
# such as rcp_fw/web_storage) — no manual offsets needed.
idf.py merge-bin -o "${OUT}"

SIZE=$(stat -c %s "${OUT}")
printf "\nMerged image: %s (%s bytes)\n" "${OUT}" "${SIZE}"

# The same build also produces the OTA bundle, so a release can offer both a factory image for
# ESPConnect and an image that is uploaded on the Firmware page of an already running device.
BUNDLE="$(dirname "${OUT}")/esp_ot_br_ota.bin"
"${PROJECT_DIR}/tools/make_ota_bundle.py" --build-dir "${BUILD_DIR}" -o "${BUNDLE}"

if [[ -n "${WIN_DOWNLOADS:-}" ]]; then
    cp "${OUT}" "${BUNDLE}" "${WIN_DOWNLOADS}/"
    echo "Copied to ${WIN_DOWNLOADS}/"
fi

echo
echo "Next: open https://thelastoutpostworkshop.github.io/ESPConnect/ in Chrome or Edge,"
echo "      connect the board over USB, pick Flash Tools -> Flash Firmware,"
echo "      select ${OUT##*/}, offset 0x0, 'Erase entire flash before writing' = on."
echo
echo "The merged image contains the bootloader, partition table, otadata, app,"
echo "web GUI (web_storage) and RCP firmware, so nothing else has to be flashed."
echo "Erasing the flash also clears NVS, so the Wi-Fi credentials and Thread"
echo "dataset have to be configured again through the ESP-ThreadBR-XXXX setup AP."