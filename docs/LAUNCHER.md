# Installing M5MonsterC5-Tab5 with M5StackLauncher

[M5StackLauncher](https://github.com/joshuawowk/M5StackLauncher) turns the Tab5 into a
multi-firmware device: it keeps itself in a small app slot, repartitions the flash on
demand, and installs other firmware alongside it. This document covers packaging
M5MonsterC5-Tab5 for it.

## The artifact

| File | What it is |
| --- | --- |
| `M5MonsterC5-Tab5-launcher.bin` | **Install this via the Launcher.** Flash-offset image: bootloader @ `0x2000`, partition table @ `0x8000`, app @ `0x10000`. |
| `launcher-manifest.json` | Machine-readable record of the install layout (offsets, sizes, sha256), modelled on LauncherHub's `install` object. Documentation only — not needed for SD or Favorites installs, and LauncherHub submissions go through its own service. |
| `M5MonsterC5-Tab5-launcher.bin.sha256` | Checksum. |

`M5MonsterC5-Tab5-launcher.bin` is byte-identical to `M5MonsterC5-Tab5-full.bin` from the
same build — same merge, same inputs. It exists under its own name so the SD-card asset is
unambiguous in a release listing, and so the Favorites link below can point at a stable
`releases/latest/download/` URL. Either file works, and both are equally valid for a
direct esptool flash at `0x0`.

Do **not** hand the Launcher `M5MonsterC5-Tab5.bin` (the bare app). Without a partition
table at `0x8000` the Launcher falls back to treating the whole file as a raw app image
and will not create the `storage` data partition.

## Building it

```bash
idf.py build                                  # produces build/ + binaries-esp32p4/
python3 tools/package_launcher.py --build-dir build
```

Or validate an image you already have:

```bash
python3 tools/package_launcher.py --check binaries-esp32p4/M5MonsterC5-Tab5-launcher.bin
```

The script merges the three components and then replays the Launcher's own
`updateFromSD()` parser against the result — partition-table magic, app entry lookup,
app-size resolution, data-partition sizing, total flash budget. It exits non-zero if the
Launcher would reject the image, so CI cannot ship a broken package. The
`Package for M5StackLauncher` job in `.github/workflows/esp32p4-build-master.yml` runs it
on every release build.

## Installing on the Tab5

**From the SD card** (recommended):

1. Copy `M5MonsterC5-Tab5-launcher.bin` to the SD card (root, or any folder).
2. Insert the card, boot the Launcher, choose **SD Card**, browse to the file, select it.
3. The Launcher repartitions, flashes, and reboots into the app. No `SPIFFS` prompt
   appears: that prompt is only shown for a partition literally labelled `spiffs` that
   carries a payload, and this image has neither.

**From a URL** — add a Favorites entry to `config.conf` on the SD card:

```json
"favorite": [
  {
    "name": "M5MonsterC5-Tab5",
    "fid": "",
    "link": "https://github.com/C5Lab/M5MonsterC5-Tab5/releases/latest/download/M5MonsterC5-Tab5-launcher.bin"
  }
]
```

Leave `fid` blank for a direct link. This is a **different code path** from the SD-card
install: `installExtFirmware()` fetches bytes `0x8000`–`0xFFFF` with an HTTP Range request,
reads the partition table straight out of that response, and streams the install — it does
not stage the file on the SD card first. The host must therefore answer Range requests
with `206 Partial Content`; GitHub release downloads do, but a self-hosted server may need
configuring.

This firmware does not chain back to the Launcher on its own; see the Launcher's own
documentation for how to return to it on the Tab5.

## What the Launcher does with the image

Measured from the current build:

```
partition table @ 0x8000
  nvs             type=0x01 sub=0x02  off=0x009000  size=0x006000
  phy_init        type=0x01 sub=0x01  off=0x00F000  size=0x001000
  factory         type=0x00 sub=0x00  off=0x010000  size=0xA00000
  human_face_det  type=0x01 sub=0x82  off=0xA10000  size=0x064000
  storage         type=0x01 sub=0x82  off=0xA74000  size=0x200000

app   'factory' @ 0x010000, 2,698,944 B
data  'human_face_det'  declared 0x64000  -> Launcher creates 0x70000, empty
data  'storage'         declared 0x200000 -> Launcher creates 0x70000, empty  <-- shrunk
~4.95 MB of 16 MB flash after install (incl. the Launcher's own 0x180000 slot)
```

Three things are worth knowing:

- **The 10 MB `factory` declaration is intentional and harmless.** The declared partition
  is larger than the file, so the Launcher takes everything after `0x10000` as the app
  (its "tail size" fallback) and sizes the real partition to the ~2.7 MB image. Shrinking
  `partitions.csv` would change standalone flashing for no gain here.
- **`human_face_det` is dead weight.** It is an M5 demo leftover with no references in
  `main/` or `components/`; the Launcher still reserves 448 KB for it because it appears
  in the table. Removing that row from `partitions.csv` would reclaim it. Left alone here
  because it is a firmware change, not a packaging one.
- **`storage` is created at 448 KB, not the declared 2 MB.** Both SPIFFS partitions ship
  empty, and the Launcher gives an empty data partition below its threshold
  (`LAUNCHER_DEFAULT_SPIFFS_THRESHOLD`, 0xC00000 on the Tab5) the default size
  `LAUNCHER_DEFAULT_SPIFFS_SIZE` = 0x70000 instead of the declared size. This is harmless
  today because nothing in the firmware mounts SPIFFS — `bsp_spiffs_mount()` exists in
  `components/m5stack_tab5` but is never called. **If the app ever starts using
  `storage`, it will only have 448 KB under a Launcher install**; shipping a real SPIFFS
  payload in the image is what makes the Launcher honour the declared size.

## Bootloader compatibility — verify on hardware

The Launcher does **not** replace the bootloader when it installs an app. This firmware
therefore runs under the Launcher's Arduino-built bootloader, not the one merged into
`M5MonsterC5-Tab5-launcher.bin` (that copy is only used when the image is flashed
directly at `0x0` with esptool).

The two builds differ in bootloader-time configuration:

| Setting | This project | Launcher |
| --- | --- | --- |
| Board definition | ESP32-P4, IDF 5.4.1, Tab5 BSP | `board = esp32p4_es` → `boards/_jsonfiles/esp32p4_es.json`, "Espressif ESP32-P4 (**generic**, ES pre rev.300)" |
| Flash mode in image header | `DIO` @ 80 MHz, 16 MB | board JSON declares `qio` |
| PSRAM | `SPIRAM_MODE_HEX`, `SPIRAM_SPEED_200M`, `SPIRAM_XIP_FROM_PSRAM` | `psram_mode: oct`, `f_psram: 200 MHz` |
| L2 cache | `CACHE_L2_CACHE_256KB`, `LINE_128B` | not declared |

Note the board row: the Launcher's Tab5 environment builds against a **generic** P4 board
definition (the Tab5-specific JSON was folded into it upstream), with Tab5 specifics
supplied as `-D` flags in `boards/m5stack-tab5/platformio.ini`. So the PSRAM and flash
values below are that generic file's defaults, not Tab5-verified choices.

Read the flash-mode row carefully: `sdkconfig` selects `CONFIG_ESPTOOLPY_FLASHMODE_QIO`,
but `CONFIG_ESPTOOLPY_FLASHMODE` resolves to `"dio"` and both the bootloader and app
headers in the shipped image are stamped **DIO** — QIO is negotiated by the bootloader at
runtime. That is normal for IDF on the P4 and is *not* itself a mismatch; it just means
the header value is not the thing to compare against the Launcher's `qio`.

The rows that could matter are PSRAM mode — this project builds `HEX`/16-line, the generic
board file says `oct` — and L2 cache, which is bootloader-configured on the P4 and not
declared at all. Neither is strong evidence of a real mismatch, precisely because they are
generic defaults rather than deliberate Tab5 settings. Whether either bites depends on how
much of that setup the Arduino bootloader defers to the app, which cannot be settled by
reading configs.

**So: confirm the first install on hardware.** If the app hangs immediately after the
Launcher reboots into it, this is the first thing to check. Flashing
`M5MonsterC5-Tab5-launcher.bin` directly at `0x0` with esptool uses this project's own
bootloader and isolates the question — if it boots that way but not via the Launcher, the
bootloader is the cause.

## The C6 co-processor is separate

`wifi_c6_fw/` holds the ESP-Hosted slave firmware for the Tab5's onboard ESP32-C6. It
lives on a different chip and is not part of the Launcher package — flash it once with
`wifi_c6_fw/flash.sh`. Likewise the MonsterC5 add-on board runs JanOS, flashed via
<https://c5lab.github.io/projectZero/janos_flash.html>.
