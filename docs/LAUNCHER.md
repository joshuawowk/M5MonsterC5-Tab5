# Installing M5MonsterC5-Tab5 with M5StackLauncher

[M5StackLauncher](https://github.com/joshuawowk/M5StackLauncher) turns the Tab5 into a
multi-firmware device: it keeps itself in the factory slot, installs each firmware into its
own OTA partition, and boots whichever one you pick. This document covers packaging
M5MonsterC5-Tab5 for it, installing it, and how switching between apps works.

## The artifact

| File | What it is |
| --- | --- |
| `MonsterC5-Tab5.bin` | **Install this via the Launcher.** Flash-offset image: bootloader @ `0x2000`, partition table @ `0x8000`, app @ `0x10000`. |
| `MonsterC5-Tab5.bin.sha256` | Checksum. |
| `launcher-manifest.json` | Machine-readable record of the install layout (offsets, sizes, sha256, app version), modelled on LauncherHub's `install` object. Documentation only — not needed for SD or Favorites installs, and LauncherHub submissions go through its own service. |

**The file name is the app's name on the device.** An SD-card install is listed under the
file name minus `.bin`, cut to 20 characters (`launcherAppNameFromFile()`); its partition
label is the first six letters and digits, lowercased (`monste`), and its menu icon the first
five characters (`MONST`). A longer name such as `M5MonsterC5-Tab5-launcher.bin` would show
up as `M5MonsterC5-Tab5-lau`. Rename the file before copying it, or use *Rename App* in the
Launcher, if you want something else.

**It is `M5MonsterC5-Tab5-full.bin` minus two partition-table rows.** Same bootloader, same
app, but the embedded table drops the `human_face_det` and `storage` SPIFFS rows and has its
MD5 row re-signed (the result matches IDF's `gen_esp32part.py` byte for byte), so the file
still flashes and boots directly at `0x0` with esptool. The rows go because of what the
Launcher does with them:

- every FAT/SPIFFS/LittleFS row becomes a real partition — 448 KB each here, since both ship
  empty;
- a partition whose label already exists is **reused**, so the generic `storage` would be
  shared with any other installed firmware that uses the same label;
- deleting the app **erases** its linked SPIFFS partition, shared or not — and only the last
  SPIFFS row gets linked, so `human_face_det` would be left behind as dead space.

This firmware mounts neither partition (`bsp_spiffs_mount()` is never called). If it ever
starts using `storage`, package with `--keep-data storage`, and expect the Launcher to create
an empty SPIFFS partition at its default 448 KB rather than the declared 2 MB unless the image
carries a payload for it.

With no data rows left, the bare `M5MonsterC5-Tab5.bin` would install the same way from SD —
the Launcher takes a file with no table at `0x8000` as a raw app image — but
`MonsterC5-Tab5.bin` is the file the validator checks and the release links point at.

## Building it

```bash
idf.py build
python3 tools/package_launcher.py --build-dir build   # -> binaries-esp32p4/MonsterC5-Tab5.bin
```

Use `--build-dir build` for local builds. The default, `binaries-esp32p4/`, is refreshed by a
stamp-gated post-build step and can hold an app several commits older than `build/`; the
script warns when the two disagree.

Validate an image you already have:

```bash
python3 tools/package_launcher.py --check binaries-esp32p4/MonsterC5-Tab5.bin
```

The script merges the three components, strips the data rows, and then replays the Launcher's
own `updateFromSD()` parser against the result — partition-table magic, app entry lookup,
app-size resolution, data-partition sizing. It reports the name the app will get and the flash
it needs beside the Launcher, and exits non-zero if the Launcher would reject the image, so CI
cannot ship a broken package. The `Package for M5StackLauncher` step in
`.github/workflows/esp32p4-build-master.yml` runs it on every release build, and
`MonsterC5-Tab5.bin` is uploaded as a standalone release asset.

## Installing on the Tab5

**From the SD card:**

1. Copy `MonsterC5-Tab5.bin` to the SD card (root, or any folder).
2. Boot the Launcher, choose **SD**, browse to the file, and select it.
3. The Launcher creates an OTA partition for the app (about 2.6 MB for v1.5.2), selects it for
   boot, and reboots into it. No SPIFFS prompt appears — the image has no data rows.

**Over USB, without an SD card** — convenient for development builds:

```bash
python3 tools/launcher_serial_install.py                        # build/M5MonsterC5-Tab5.bin via /dev/ttyACM0
python3 tools/launcher_serial_install.py binaries-esp32p4/MonsterC5-Tab5.bin --monitor 30
```

The Tab5 must be sitting in the Launcher (boot screen or menu), not in an app. The script
drives the Launcher's serial console command `flash firmware <name> <size>`: the Launcher
creates the OTA partition, the script streams the app image in 2048-byte chunks with an ACK
per chunk, and the Launcher writes the table, selects the app, and reboots into it. Only the
app image is sent — the same bytes an SD install copies out of `MonsterC5-Tab5.bin` — and
either a bare app or the merged image is accepted. `--monitor` follows the device's output
through the reboot. The script needs pyserial and falls back to an ESP-IDF Python that has it.

Reinstalling under a name that is already installed adds a second copy (label `monst1`, and so
on) instead of replacing the first; delete the old one in the Launcher first.

**From a URL** — add a Favorites entry to `config.conf` on the SD card:

```json
"favorite": [
  {
    "name": "MonsterC5-Tab5",
    "fid": "",
    "link": "https://github.com/C5Lab/M5MonsterC5-Tab5/releases/latest/download/MonsterC5-Tab5.bin"
  }
]
```

Leave `fid` blank for a direct link. This is a **different code path** from the SD-card
install: `installExtFirmware()` fetches bytes `0x8000`–`0xFFFF` with an HTTP Range request,
reads the partition table straight out of that response, and streams the install — it does
not stage the file on the SD card first. The host must therefore answer Range requests with
`206 Partial Content`; GitHub release downloads do, but a self-hosted server may need
configuring. The link only resolves once a release that includes `MonsterC5-Tab5.bin` has been
published.

## Switching between apps

What boots is decided by the Launcher's bootloader — a patched `bootloader_start.c` from the
Launcher's lib builder (bmorcelli/myLibBuilder, branch `launcher_keyboot`), not this project's
bootloader:

| Reset | Boots |
| --- | --- |
| Power-on (power button) | the Launcher |
| Wake from deep sleep, main-watchdog core reset | the Launcher, unless its skip-on-deep-sleep option (`DDLB`) is set |
| Anything else — software restart, crash reboot, esptool / USB-JTAG reset | the app selected in `otadata` |

That table comes from the bootloader source; the software-restart and USB-JTAG-reset rows were
also observed on a Tab5. In practice:

- **Launch the app** from the Launcher menu: *MonsterC5-Tab5 → Launch*. On the boot screen you
  can also tap its shortcut card, or press its digit on a keyboard. With the Launcher's
  boot-to-app setting on, doing nothing boots the last app when the countdown ends; with it
  off, the Launcher stays in its menu.
- **The app's own restarts stay in the app** — the orientation toggle's `esp_restart()`, for
  instance. A crash reboot does too, so a firmware that crashes on start keeps rebooting
  itself until you power-cycle.
- **Get back to the Launcher** by powering the Tab5 off and on.

The Tab5 Launcher reboots by power-cycling the device through its RTC alarm, which the
bootloader sees as a power-on. To still land in the app, the Launcher sets an NVS flag
(`launcher/init`) on every start and, when it finds the flag already set, clears it and does a
software restart instead — so launching an app briefly passes through the Launcher. One side
effect, per the Launcher's `main.cpp`: powering off from inside the Launcher leaves the flag
set, so the next power-on goes straight to the selected app; power-cycle once more to reach
the Launcher.

## Running under the Launcher's bootloader

The Launcher does not replace the bootloader when it installs an app, so this firmware (IDF
5.4.1) runs under the Launcher's bootloader (IDF 5.5.4) and inside the Launcher's partition
table — not the ones merged into `MonsterC5-Tab5.bin`, which are only used when the file is
flashed directly at `0x0`.

Checked in source:

- **L2 cache and PSRAM** are set up by the app itself during startup (IDF 5.4.1's
  `cpu_start.c` calls `esp_config_l2_cache_mode()`, and PSRAM is initialised by the app), so
  the bootloader's configuration does not carry over.
- **Partitions:** the app uses `nvs`, which the Launcher's layout provides. It does not read
  `phy_init` (the P4 has no radio; Wi-Fi is on the C6) or write core dumps
  (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`), so neither the missing `phy_init` row nor the
  Launcher's `coredump` partition matters.

Verified on a Tab5 (Launcher `dev`, app `v1.5.2-22-g9797b98`, installed over USB with
`launcher_serial_install.py`):

- the Launcher created `monste` (ota_0, `0x2A0000`) and selected it for boot — the app
  partition size the validator predicts;
- the app booted: 32 MB PSRAM at 200 MHz, memory test OK, `.text`/`.rodata` executing from
  PSRAM, `app_main()` running, and no resets or panics across about two minutes of uptime; a
  USB-JTAG reset booted it again;
- the Launcher's NVS came through intact — its settings, saved Wi-Fi networks and app
  registry (`l_apps`: `monste = MonsterC5-Tab5`), in NVS format v2.

That test did not exercise the SD-card install path itself. That path is covered by the
validator, which mirrors `updateFromSD()` and was re-checked against the Launcher source in
September 2026; the bytes it copies into the app partition are the ones the USB install wrote.

**Shared NVS.** The app shares the Launcher's 20 KB `nvs` partition (the Launcher's layout,
not this project's 24 KB row) with the Launcher's settings, saved Wi-Fi networks and app
registry, plus the boot options its bootloader reads. There is room — on the test device the
active page was almost empty — but `app_main()` erases NVS if `nvs_flash_init()` reports no
free pages or a newer format, which would reset all of that (installed apps would still boot,
listed under their partition labels). The app's first-boot `NVS not available, using default`
log lines are normal: its settings namespace does not exist until it saves something.

## The C6 co-processor is separate

`wifi_c6_fw/` holds the ESP-Hosted slave firmware for the Tab5's onboard ESP32-C6. It
lives on a different chip and is not part of the Launcher package — flash it once with
`wifi_c6_fw/flash.sh`. Likewise the MonsterC5 add-on board runs JanOS, flashed via
<https://c5lab.github.io/projectZero/janos_flash.html>.
