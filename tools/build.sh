#!/usr/bin/env bash
# Builds TDisplay_Suite for one board, and flashes it when a port is given.
#   tools/build.sh s3  [port] [extra -D flags]   LilyGO T-Display-S3 (default port /dev/ttyACM0)
#   tools/build.sh cyd [port] [extra -D flags]   Freenove 2.8" CYD   (default port /dev/ttyUSB0)
#   tools/build.sh cyd35 [port] [extra -D flags] 3.5" ESP32-3248S035C (default port /dev/ttyUSB0)
# Use "-" as the port to build only. Output goes to build/<board>/.
#
# The CYD build sets TFT_eSPI up from cyd_flags.txt (USER_SETUP_LOADED), so the
# T-Display setup picked in User_Setup_Select.h is left alone.
set -euo pipefail
cd "$(dirname "$0")/.."
board=${1:-s3}; port=${2:-}; extra=${3:-}
case "$board" in
  s3)  fqbn="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB"
       flags="$extra"; port=${port:-/dev/ttyACM0} ;;
  cyd) fqbn="esp32:esp32:esp32:PartitionScheme=min_spiffs"
       flags="$(cat tools/cyd_flags.txt) $extra"; port=${port:-/dev/ttyUSB0} ;;
  cyd35) fqbn="esp32:esp32:esp32:PartitionScheme=min_spiffs"
       flags="$(cat tools/cyd35_flags.txt) $extra"; port=${port:-/dev/ttyUSB0} ;;
  *)   echo "board must be s3, cyd or cyd35"; exit 1 ;;
esac
args=(compile --fqbn "$fqbn" --output-dir "build/$board" --build-property "compiler.cpp.extra_flags=$flags")
[ "$port" != "-" ] && args+=(--upload -p "$port")
arduino-cli "${args[@]}" TDisplay_Suite
