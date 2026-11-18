/*
 * Sentinel Bootloader "Update from GitHub" descriptor.
 *
 * Emits the fixed 256-byte sentinel_app_info_t block into .rodata_custom_desc, right after
 * esp_app_desc_t (image offset SENTINEL_APP_INFO_OFFSET = 0x120). Sentinel Bootloader reads it
 * straight out of the installed image and offers an in-launcher update from the named GitHub
 * repository's latest release, however this firmware was installed. See sentinel-bootloader
 * docs/firmware-authors.md ("Say where your firmware lives") and the vendored
 * components/sentinel_app/include/sentinel_app.h.
 *
 * Repository: joshuawowk/M5MonsterC5-Tab5 (this fork), NOT C5Lab upstream. The device runs this
 * fork's firmware, which carries work upstream does not (no-SD UART capture streaming, the A164
 * keyboard, the JanOS version gate); tracking C5Lab releases would replace that with upstream.
 * To retarget updates at upstream, change the URL below.
 *
 * Asset: "M5MonsterC5-Tab5.bin". The release (published by .github/workflows/esp32p4-build-master.yml
 * on a push to main) carries two app .bin files -- "M5MonsterC5-Tab5.bin" (the bare app image,
 * magic E9) and "MonsterC5-Tab5.bin" (a merged image for bmorcelli/Launcher). Sentinel installs
 * the bare app image: it needs nothing beyond a default NVS (no SPIFFS is mounted, and the P4 has
 * no radio so phy_init is unused), so a bare image is the cleanest Sentinel install. An empty
 * asset string would be ambiguous here because several assets name the Tab5. Sentinel ignores any
 * version embedded in the asset name when matching, and offers a release only when its tag is a
 * newer version than this image's PROJECT_VER (git describe).
 */
#include "sentinel_app.h"

SENTINEL_APP_INFO("https://github.com/joshuawowk/M5MonsterC5-Tab5", "M5MonsterC5-Tab5.bin");
