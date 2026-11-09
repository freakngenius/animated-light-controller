#!/bin/bash
# flash_cyd.sh - one command: installs what is missing, asks for your Wi-Fi, uploads the sketch
# to the CYD, then opens the Serial Monitor.
#
# Run it from Terminal:   ./flash_cyd.sh
# Options:  --wifi      ask for the Wi-Fi name and password again
#           --no-monitor  upload, but do not open the Serial Monitor
#           --check     install and build only, no board needed (does not upload)
#
# The Wi-Fi password is typed into this window only. It is written to secrets.h on your Mac
# and goes nowhere else.

cd "$(dirname "$0")" || exit 1

CORE_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
# Same as the Arduino IDE settings in SETUP.md: ESP32 Dev Module, upload speed 115200.
# The other settings (240 MHz, QIO, 80 MHz, 4 MB, default partitions, no PSRAM) are already the defaults.
FQBN="esp32:esp32:esp32:UploadSpeed=115200"

ASK_WIFI=0; MONITOR=1; CHECK=0
for arg in "$@"; do
  case "$arg" in
    --wifi) ASK_WIFI=1 ;;
    --no-monitor) MONITOR=0 ;;
    --check) CHECK=1 ;;
    *) echo "Unknown option: $arg"; exit 2 ;;
  esac
done

say() { printf '\n== %s\n' "$1"; }
die() { printf '\nSTOPPED: %s\n' "$1"; exit 1; }

# ---- 1. the command-line tool -------------------------------------------------------------
say "1/5  Arduino command-line tool"
if ! command -v arduino-cli >/dev/null 2>&1; then
  command -v brew >/dev/null 2>&1 || die "Homebrew is not installed. Install it from https://brew.sh, then run this again."
  brew install arduino-cli || die "Could not install arduino-cli."
fi
arduino-cli version

# ---- 2. board support ---------------------------------------------------------------------
say "2/5  ESP32 board support (2.0.17)"
if arduino-cli core list | grep -q '^esp32:esp32 *2\.0\.17'; then
  echo "already installed"
else
  arduino-cli core update-index --additional-urls "$CORE_URL" || die "Could not download the board list. Check your internet."
  arduino-cli core install esp32:esp32@2.0.17 --additional-urls "$CORE_URL" || die "Could not install ESP32 board support."
fi

# ---- 3. the one library -------------------------------------------------------------------
say "3/5  LovyanGFX library (1.3.0)"
if arduino-cli lib list | grep -q '^LovyanGFX '; then
  echo "already installed"
else
  arduino-cli lib install "LovyanGFX@1.3.0" || die "Could not install LovyanGFX."
fi

# ---- 4. Wi-Fi -----------------------------------------------------------------------------
say "4/5  Wi-Fi (secrets.h)"
[ -f secrets.h ] || cp secrets.h.example secrets.h   # fresh clone: start from the placeholder file
esc() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
if [ "$ASK_WIFI" = 1 ] || grep -q 'YOUR_WIFI_NAME' secrets.h; then
  echo "Use the same 2.4 GHz Wi-Fi network the LIFX bulb is on."
  read -r -p "Wi-Fi name: " SSID
  read -r -s -p "Wi-Fi password (typing is hidden): " PASS; echo
  [ -n "$SSID" ] || die "No Wi-Fi name entered."
  {
    echo '// secrets.h - your Wi-Fi details. Keep this file private (do not share or upload it).'
    echo '//'
    echo '// The CYD board can only join 2.4 GHz Wi-Fi (not 5 GHz). If your router has one'
    echo '// combined name for both, that is fine. Keep the quote marks.'
    echo '#pragma once'
    echo ''
    printf '#define WIFI_SSID     "%s"\n' "$(esc "$SSID")"
    printf '#define WIFI_PASSWORD "%s"\n' "$(esc "$PASS")"
  } > secrets.h || die "Could not write secrets.h."
  echo "saved to secrets.h"
else
  echo "secrets.h already has a Wi-Fi name ($(grep WIFI_SSID secrets.h | head -1 | sed 's/.*"\(.*\)".*/\1/')). Run with --wifi to change it."
fi

# ---- 5. build and upload ------------------------------------------------------------------
if [ "$CHECK" = 1 ]; then
  say "5/5  Build only (no board)"
  arduino-cli compile --fqbn "$FQBN" . || die "The build failed."
  echo; echo "Build OK. Run ./flash_cyd.sh with the board plugged in to upload."
  exit 0
fi

say "5/5  Find the board, build, upload"
PORT=$(ls /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem* 2>/dev/null | head -n 1)
if [ -z "$PORT" ]; then
  die "No board found. Plug the CYD in with a USB cable that carries data (some only charge), wait 5 seconds, run this again.
Still nothing? Try another cable or port. If it persists, install the 'WCH CH34x macOS driver', then unplug and replug."
fi
echo "board on $PORT"
arduino-cli compile --fqbn "$FQBN" --upload --port "$PORT" . || die "Build or upload failed.
If it says 'Connecting........____': hold the BOOT button on the back of the board, run this again, and let go of BOOT when the percentages start."

echo
echo "UPLOADED. On the board: tap the centre of each calibration mark the first time."
if [ "$MONITOR" = 1 ]; then
  echo "Opening the Serial Monitor. Look for:  using bulb ... at 192.168...   Press Ctrl+C to quit."
  sleep 1
  arduino-cli monitor -p "$PORT" -c baudrate=115200
fi
