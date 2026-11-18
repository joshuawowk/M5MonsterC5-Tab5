# Running M5MonsterC5-Tab5 under the Sentinel launcher

[Sentinel Bootloader](https://github.com/joshuawowk/sentinel-bootloader) is the Tab5's
current launcher. It replaced [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)
(see [LAUNCHER.md](LAUNCHER.md) for that one). Sentinel is a custom second-stage bootloader
plus a launcher app: each installed firmware gets its own private partitions carved from a
pool, and the partition table the firmware boots under is a *view* that lists only that
firmware's partitions — there is no `factory` row and no second OTA slot. The authoritative
rules are in the sentinel-bootloader repo: `docs/firmware-authors.md`, `docs/design.md`,
`docs/compatibility.md` (which has the M5MonsterC5 row).

This firmware is Sentinel-ready. What that means here:

## Installing and updating

- **Install / update over USB-C:** `python3 tools/sentinelctl.py install M5MonsterC5-Tab5.bin`
  (from the sentinel-bootloader repo), or from the SD card through the launcher. A build with
  the same project name updates the installed one in place, keeping its NVS.
- **Do NOT use this repo's `tools/launcher_serial_install.py` on a Sentinel Tab5.** It speaks
  bmorcelli/Launcher's serial `flash firmware` protocol, which Sentinel does not implement.
- **Which image:** ship the **bare app image** `M5MonsterC5-Tab5.bin` (magic `E9`). Sentinel
  gives a bare image an app slot plus a default NVS, which is all this firmware needs — it
  mounts no SPIFFS and the P4 has no radio, so `phy_init` is unused. The merged
  `MonsterC5-Tab5.bin` (for bmorcelli/Launcher) and `M5MonsterC5-Tab5-full.bin` also install,
  but the bare image is the clean choice.

## Update from GitHub (the SNAP descriptor)

The image carries a fixed 256-byte Sentinel descriptor at offset `0x120`
(`components/sentinel_app/`, invoked in `main/sentinel_app_info.c`). Sentinel reads it and
offers an in-launcher update from this fork's latest GitHub release:

- Repository: **`joshuawowk/M5MonsterC5-Tab5`** (this fork), not C5Lab upstream. The device
  runs this fork's firmware, which carries work upstream lacks (no-SD UART capture streaming,
  the A164 keyboard, the JanOS version gate, the CH343-bridge USB-A fixes); pointing updates
  at C5Lab would replace all of that. To retarget at upstream, change the URL in
  `main/sentinel_app_info.c`.
- Release asset: `M5MonsterC5-Tab5.bin`. Updates are offered only when the release tag is a
  newer version than the running image's `PROJECT_VER` (`git describe`), so a dev build ahead
  of the latest tag is not offered a downgrade.
- `tools/package_launcher.py` decodes and prints this descriptor in its report, and warns if
  it is missing or malformed, so CI catches a build that dropped it.

## Returning to the launcher

**Settings → Return to Sentinel** blanks `otadata` and restarts, which Sentinel reads as a
request for the launcher (`esp_ota_set_boot_partition(factory)` does nothing under Sentinel —
there is no factory row). When the image is flashed standalone (no `otadata` partition) the
button is simply a restart. A chip reset — the reset button, a power cycle — always returns to
the launcher as well, and three crashes within ~30 s of boot trip Sentinel's crash-loop guard.

## The shared ESP32-C6 radio

The Tab5's onboard ESP32-C6 runs **one** ESP-Hosted slave firmware shared by every app, which
Sentinel expects at 2.12.x. This firmware **never flashes the C6** — it brings esp-hosted up
with the Tab5's SDIO pins and resets the C6 on GPIO15, but leaves its firmware alone. Do not
run `wifi_c6_fw/flash.sh` on a Sentinel-managed Tab5: it writes an older slave image and can
break Wi-Fi for Sentinel and every other app. (The firmware's radio for Wi-Fi attacks is the
**external** ESP32-C5 "Monster" board on USB-A running JanOS — a different chip entirely.)

## The SD card

The Tab5's microSD is mounted in SDMMC mode, with an **SPI-mode fallback** (added for Sentinel)
if the SDMMC mount times out — which happens when a previously-run firmware left the card in
SPI mode and the card was not power-cycled. See `components/m5stack_tab5/m5stack_tab5.c`
(`bsp_sdcard_init`). *Needs a hardware check:* a card left in SPI mode by another app should
now mount on the fallback.

## What a firmware finds on start (Sentinel specifics)

Sentinel starts a firmware with a digital-system reset of the P4 only, not a power cycle, so
external chips (IO expanders, panel, touch, codec, SD, the C6) keep the launcher's state. This
firmware already: drives USB-A 5V (IO expander 0x44, P3) on itself at boot; tolerates an empty
NVS on first start (every setting has a default); and does not self-OTA (there is no second app
slot — the only GitHub OTA path reflashes the *external* C5, not the Tab5 app). See
`docs/compatibility.md` in sentinel-bootloader for the on-device verification.
