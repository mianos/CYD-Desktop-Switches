#!/usr/bin/env bash
# Build wrapper: ./build.sh [flash|monitor|ota <host>]. Sets the esp32 target on first run.
set -e
set -o pipefail
export IDF_PATH=/Users/robertfowler/.espressif/v6.0.1/esp-idf
. "$IDF_PATH/export.sh" >/dev/null 2>&1
cd "$(dirname "$0")"
PORT=${PORT:-/dev/tty.usbserial-14320}
if [ ! -f sdkconfig ] || ! grep -q 'CONFIG_IDF_TARGET="esp32"' sdkconfig 2>/dev/null; then
    idf.py set-target esp32
fi
idf.py build 2>&1 | tee /tmp/cyd_build.log | tail -n 40
case "$1" in
    flash) idf.py -p "$PORT" flash ;;
    monitor) idf.py -p "$PORT" flash monitor ;;
    ota)
        # Push the freshly built image to a running panel over the LAN:
        #   ./build.sh ota 192.168.1.50      (or OTA_HOST=desk ./build.sh ota)
        # The device streams it into the inactive OTA slot, reboots into it and
        # marks it valid once Wi-Fi comes back (otherwise it rolls back).
        HOST=${OTA_HOST:-${2:-}}
        if [ -z "$HOST" ]; then
            echo "usage: ./build.sh ota <ip-or-host>   (or set OTA_HOST)" >&2
            exit 1
        fi
        # 'Expect:' off: esp_http_server never sends 100-continue, so curl would
        # otherwise stall for a second before uploading the body.
        curl -fsS -H 'Expect:' --data-binary "@build/cyd_display_test.bin" "http://$HOST/firmware"
        echo
        ;;
esac
