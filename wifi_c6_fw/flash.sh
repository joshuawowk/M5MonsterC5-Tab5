#!/usr/bin/env bash
#
# Flash the ESP-Hosted slave firmware to the Tab5's onboard ESP32-C6.
#
# WARNING - Sentinel Bootloader: the C6 runs ONE shared slave firmware for EVERY app
# installed on the Tab5. Sentinel and the firmware built for it expect esp-hosted 2.12.x
# on the C6. This script writes the bundled 1.4.1 image, which REPLACES that shared slave
# and can break Wi-Fi in Sentinel and every other installed firmware until a 2.12.x image
# is restored (no in-app way back is shipped here). Only run this on a Tab5 that is NOT
# managed by the Sentinel launcher, or when you specifically intend to change the shared
# C6 firmware. See docs/SENTINEL.md.
#
set -e
cat <<'EOF'
This flashes the Tab5's shared C6 radio firmware (ESP-Hosted slave 1.4.1).
Under the Sentinel launcher the C6 is shared by every app and is expected to run
2.12.x; overwriting it can break Wi-Fi for Sentinel and every other Tab5 app.
See docs/SENTINEL.md before continuing.
EOF
read -r -p "Type 'yes' to flash the C6 anyway: " ans
if [ "$ans" != "yes" ]; then
    echo "Aborted."
    exit 1
fi
esptool.py write_flash 0x0 ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin
