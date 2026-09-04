#!/usr/bin/env python3
"""Package M5MonsterC5-Tab5 for installation by M5StackLauncher.

The Launcher installs a firmware from the SD card by seeking to 0x8000 in the
.bin, reading the embedded partition table, locating the app entry, and copying
the app + data payloads into partitions it creates itself.  That means the file
we ship has to be a *flash-offset-based* image (offset 0 == flash address 0),
exactly like the one esptool's ``merge_bin`` produces:

    0x02000  bootloader.bin        (ESP32-P4 bootloader offset)
    0x08000  partition-table.bin   (Launcher reads this)
    0x10000  M5MonsterC5-Tab5.bin  (the app)

This script builds that image and then *validates* it by replicating the exact
parsing that ``updateFromSD()`` in the Launcher performs, so CI fails loudly
instead of shipping a binary the Launcher silently rejects.

Reference: https://github.com/joshuawowk/M5StackLauncher
           src/sd_functions.cpp        :: updateFromSD, measureSdEspImage,
                                          sdPartitionIsEmpty, boundedSdPartitionPayload
           src/partition_table_model.* :: launcherPartitionInitDefaultSizes,
                                          launcherPartitionBoundedPayloadSize
           include/pre_compiler.h      :: LAUNCHER_DEFAULT_SPIFFS_THRESHOLD

Fidelity: the sizing constants are board- and runtime-dependent (see the block below)
and are hardcoded here for the Tab5's 16 MB flash -- re-check them against the Launcher
source if this is ever reused for another board. Two deliberate divergences: an app
entry that computes to size 0 makes this tool fail, where the Launcher would go on to
consider later app entries; and a table whose first entry is not 0xAA50 ends the scan
here rather than continuing. Both are stricter than the Launcher, which is the right
bias for a packaging gate.

Usage:
    tools/package_launcher.py                      # merge from binaries-esp32p4/
    tools/package_launcher.py --build-dir build    # merge straight from build/
    tools/package_launcher.py --check FILE.bin     # validate an existing image
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# --- ESP32-P4 flash layout -------------------------------------------------
BOOTLOADER_OFFSET = 0x2000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000  # LAUNCHER_PARTITION_TABLE_SIZE (partition_table_model.h)
PARTITION_ENTRY_SIZE = 32
DEFAULT_APP_OFFSET = 0x10000
FLASH_SIZE = 16 * 1024 * 1024

# --- Launcher constants -----------------------------------------------------
# LAUNCHER_DEFAULT_SPIFFS_SIZE is set at runtime by launcherPartitionInitDefaultSizes()
# (src/partition_table_model.cpp): 0x20000 for <=4 MB flash, 0x70000 above it.
# The Tab5 has 16 MB, so 0x70000 applies.
LAUNCHER_DEFAULT_SPIFFS_SIZE = 0x70000
# LAUNCHER_DEFAULT_SPIFFS_THRESHOLD defaults to 0xC00000 in include/pre_compiler.h;
# the Tab5 board config does not override it.
LAUNCHER_DEFAULT_SPIFFS_THRESHOLD = 0xC00000
LAUNCHER_APP0_SIZE = 0x180000                 # Launcher's own app slot on Tab5

APP_SUBTYPES = (0x00, 0x10, 0x20)  # factory, ota_0, ota_1 -- what updateFromSD accepts
SUBTYPE_NAMES = {0x81: "FAT", 0x82: "SPIFFS", 0x83: "LittleFS"}


class PackagingError(RuntimeError):
    pass


# ---------------------------------------------------------------- merging --

def find_esptool() -> list[str] | None:
    """Return an argv prefix that runs esptool, or None if it isn't installed."""
    candidates = [[sys.executable, "-m", "esptool"]]
    candidates += [
        [str(p), "-m", "esptool"]
        for p in sorted(Path.home().glob(".espressif/python_env/*/bin/python"))
    ]
    for argv in candidates:
        try:
            probe = subprocess.run(
                argv + ["version"], capture_output=True, timeout=60, check=False
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if probe.returncode == 0:
            return argv
    return None


def merge_python(parts: list[tuple[int, Path]], out: Path) -> None:
    """Fallback merge: place each component at its flash offset, pad with 0xFF.

    esptool's merge_bin does the same thing for an unencrypted image; it only
    additionally rewrites the bootloader's flash-size/mode header bits, which
    are already correct here because the bootloader was built from this
    project's own sdkconfig.
    """
    end = max(off + p.stat().st_size for off, p in parts)
    buf = bytearray(b"\xff" * end)
    for off, path in parts:
        blob = path.read_bytes()
        buf[off:off + len(blob)] = blob
    out.write_bytes(bytes(buf))


def merge_image(build_dir: Path, out: Path, app_name: str) -> Path:
    app = build_dir / app_name
    bootloader = build_dir / "bootloader.bin"
    table = build_dir / "partition-table.bin"
    # `build/` nests these one level deeper than `binaries-esp32p4/` does.
    if not bootloader.exists():
        bootloader = build_dir / "bootloader" / "bootloader.bin"
    if not table.exists():
        table = build_dir / "partition_table" / "partition-table.bin"

    missing = [str(p) for p in (bootloader, table, app) if not p.exists()]
    if missing:
        raise PackagingError("missing build artifact(s):\n  " + "\n  ".join(missing))

    parts = [
        (BOOTLOADER_OFFSET, bootloader),
        (PARTITION_TABLE_OFFSET, table),
        (DEFAULT_APP_OFFSET, app),
    ]

    esptool = find_esptool()
    if esptool:
        cmd = esptool + [
            "--chip", "esp32p4", "merge_bin", "-o", str(out),
            "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
        ]
        for off, path in parts:
            cmd += [hex(off), str(path)]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            raise PackagingError(f"esptool merge_bin failed:\n{proc.stderr.strip()}")
        print(f"  merged with esptool ({' '.join(esptool)})")
    else:
        merge_python(parts, out)
        print("  merged with built-in packer (esptool not found)")
    return out


# ------------------------------------------------- Launcher-side parsing ---

def measure_esp_image(data: bytes, offset: int) -> int | None:
    """Replicate Launcher measureSdEspImage(): walk segments to find real size.

    Returns the byte length of the ESP app image starting at `offset`, or None
    if the header doesn't look like a valid image.
    """
    if offset + 24 > len(data) or data[offset] != 0xE9:
        return None
    segment_count = data[offset + 1]
    hash_appended = data[offset + 23] == 1
    if segment_count == 0 or segment_count > 16:
        return None
    cursor = offset + 24  # segment headers start right after the image header
    for _ in range(segment_count):
        if cursor + 8 > len(data):
            return None
        seg_len = struct.unpack_from("<I", data, cursor + 4)[0]
        cursor += 8
        if seg_len > len(data) or cursor + seg_len > len(data):
            return None
        cursor += seg_len
    end = ((cursor + 15) // 16) * 16 + 1
    if hash_appended:
        end += 32
    end = ((end + 15) // 16) * 16
    if end <= offset or end > len(data):
        return None
    return end - offset


def partition_is_empty(data: bytes, offset: int) -> bool:
    """Launcher sdPartitionIsEmpty() (sd_functions.cpp).

    Only the first 16 bytes are inspected, and they must be *uniformly* 0xFF or
    *uniformly* 0x00 -- a mix of the two counts as carrying payload.
    """
    if offset == 0 or len(data) <= offset:
        return True
    window = data[offset:offset + 16]
    return all(b == 0xFF for b in window) or all(b == 0x00 for b in window)


def bounded_payload_size(data: bytes, offset: int, declared: int, max_size: int) -> int:
    """Launcher boundedSdPartitionPayload() + launcherPartitionBoundedPayloadSize()."""
    if offset == 0 or len(data) <= offset or declared == 0:
        return 0
    available = len(data) - offset
    copy_size = declared
    if max_size > 0 and copy_size > max_size:
        copy_size = max_size
    if copy_size > available:
        copy_size = available
    return copy_size


def parse_partition_table(data: bytes) -> list[dict]:
    entries = []
    for i in range(0, PARTITION_TABLE_SIZE, PARTITION_ENTRY_SIZE):
        base = PARTITION_TABLE_OFFSET + i
        if base + PARTITION_ENTRY_SIZE > len(data):
            break
        raw = data[base:base + PARTITION_ENTRY_SIZE]
        if raw[:2] in (b"\xeb\xeb", b"\xff\xff"):
            break
        if raw[:2] != b"\xaa\x50":
            break
        ptype, subtype = raw[2], raw[3]
        offset, size = struct.unpack_from("<II", raw, 4)
        label = raw[12:28].rstrip(b"\x00").decode("utf-8", "replace")
        entries.append(
            {"type": ptype, "subtype": subtype, "offset": offset,
             "size": size, "label": label}
        )
    return entries


def plan_install(data: bytes) -> dict:
    """Replicate updateFromSD(): what the Launcher will actually do with this file."""
    if data[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 3] != b"\xaa\x50\x01":
        raise PackagingError(
            "no partition table magic (AA 50 01) at 0x8000 -- the Launcher would "
            "treat this file as a bare app image, not a full firmware"
        )

    entries = parse_partition_table(data)
    if not entries:
        raise PackagingError("partition table at 0x8000 is empty")

    app = None
    data_parts = []
    for e in entries:
        if e["type"] == 0x00 and e["subtype"] in APP_SUBTYPES and app is None:
            declared = e["size"]
            offset = e["offset"]
            # The Launcher compares uint32_t values here; wrap so an absurd
            # declared size predicts the same branch it would take.
            if len(data) < ((declared + offset) & 0xFFFFFFFF):
                # Launcher tail-size fallback: the declared partition is bigger
                # than the file, so everything after the app offset is the app.
                size = len(data) - offset
                how = f"tail of file (declared 0x{declared:X} > file)"
            else:
                measured = measure_esp_image(data, offset)
                size = measured if (measured and measured < declared) else declared
                how = "measured image" if measured and measured < declared else "declared size"
            app = {"label": e["label"], "offset": offset, "size": size,
                   "declared": declared, "how": how}
        elif e["type"] == 0x01 and e["subtype"] in (0x82, 0x83):
            declared = e["size"]
            empty = partition_is_empty(data, e["offset"])
            label = e["label"] or "spiffs"
            if empty and declared <= LAUNCHER_DEFAULT_SPIFFS_THRESHOLD:
                create = LAUNCHER_DEFAULT_SPIFFS_SIZE
            elif label != "spiffs" and declared > LAUNCHER_DEFAULT_SPIFFS_SIZE:
                create = declared
            elif declared > LAUNCHER_DEFAULT_SPIFFS_THRESHOLD:
                create = None  # LAUNCHER_INSTALL_USE_REMAINING_SPIFFS_SIZE
            else:
                create = LAUNCHER_DEFAULT_SPIFFS_SIZE
            # The Launcher caps the copy at the partition it will create, except
            # when it sizes the partition as "use the remaining flash" (create is
            # None here), where the declared size is the cap instead.
            copy = 0 if empty else bounded_payload_size(
                data, e["offset"], declared, declared if create is None else create
            )
            data_parts.append(
                {"label": label, "kind": SUBTYPE_NAMES.get(e["subtype"], "?"),
                 "declared": declared, "create": create, "copy": copy, "empty": empty}
            )
        elif e["type"] == 0x01 and e["subtype"] == 0x81:
            data_parts.append(
                {"label": e["label"], "kind": "FAT", "declared": e["size"],
                 "create": e["size"], "copy": 0, "empty": True}
            )

    if app is None:
        raise PackagingError(
            "no app partition (type 0x00, subtype factory/ota_0/ota_1) in the "
            "table -- the Launcher would report 'Update Error.'"
        )
    if app["size"] == 0:
        raise PackagingError("computed app size is 0 -- Launcher reports 'Invalid app image'")
    if app["offset"] + app["size"] > len(data):
        raise PackagingError("app image runs past the end of the file")
    if data[app["offset"]] != 0xE9:
        raise PackagingError(
            f"no ESP image magic (0xE9) at app offset 0x{app['offset']:X}"
        )

    return {"entries": entries, "app": app, "data_parts": data_parts}


# ------------------------------------------------------------- reporting --

def report(path: Path, plan: dict) -> None:
    size = path.stat().st_size
    app = plan["app"]
    print(f"\nLauncher install plan for {path.name} ({size:,} bytes)")
    print("-" * 64)
    print("  partition table @ 0x8000:")
    for e in plan["entries"]:
        print(f"    {e['label']:<16s} type=0x{e['type']:02x} sub=0x{e['subtype']:02x} "
              f"off=0x{e['offset']:06X} size=0x{e['size']:06X}")
    print(f"\n  app  : '{app['label']}' @ 0x{app['offset']:06X}, "
          f"0x{app['size']:X} ({app['size']:,} B) via {app['how']}")

    total = LAUNCHER_APP0_SIZE + app["size"]
    for dp in plan["data_parts"]:
        create = "rest of flash" if dp["create"] is None else f"0x{dp['create']:X}"
        note = "empty (created blank)" if dp["empty"] else f"copy 0x{dp['copy']:X}"
        warn = ""
        if dp["create"] is not None and dp["create"] < dp["declared"]:
            warn = "  <-- SHRUNK from declared size"
        print(f"  data : '{dp['label']}' {dp['kind']:<8s} declared 0x{dp['declared']:X} "
              f"-> create {create}, {note}{warn}")
        total += dp["create"] or 0

    print(f"\n  estimated flash used after install: ~{total/1024/1024:.2f} MB "
          f"(Launcher app0 0x{LAUNCHER_APP0_SIZE:X} + this app + data) of 16 MB")
    if total > FLASH_SIZE:
        raise PackagingError("install layout would not fit in 16 MB of flash")
    print("  OK -- the Launcher's updateFromSD() parser accepts this image.")


def write_manifest(out_dir: Path, bin_path: Path, plan: dict, version: str) -> Path:
    """Record the resolved install layout, modelled on LauncherHub's `install` object.

    Documentation/automation aid only -- SD-card and Favorites installs read the
    layout out of the image itself, and LauncherHub submissions go through its
    own service rather than this file.
    """
    app = plan["app"]
    partitions = [{
        "type": "app", "subtype": "ota", "label": app["label"],
        "source_offset": app["offset"], "copy_size": app["size"],
        "flash_offset": app["offset"],
    }]
    for dp in plan["data_parts"]:
        partitions.append({
            "type": "data",
            "subtype": dp["kind"].lower(),
            "label": dp["label"],
            "size": dp["declared"],
            "source_offset": 0,
            "copy_size": dp["copy"],
        })

    manifest = {
        "name": "M5MonsterC5-Tab5",
        "description": "JanOS companion app for the M5MonsterC5 (ESP32-C5) add-on board",
        "author": "C5Lab",
        "category": "tab5",
        "version": version,
        "file": bin_path.name,
        "sha256": hashlib.sha256(bin_path.read_bytes()).hexdigest(),
        "install": {
            "app": {"source_offset": app["offset"], "image_size": app["size"]},
            "partitions": partitions,
            "sources": {"firmware": bin_path.name},
        },
    }
    path = out_dir / "launcher-manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    return path


def project_version() -> str:
    main_c = REPO_ROOT / "main" / "main.c"
    if main_c.exists():
        import re
        m = re.search(r'JANOS_TAB_VERSION\s+"([^"]+)"', main_c.read_text(errors="replace"))
        if m:
            return m.group(1)
    return "dev"


# ------------------------------------------------------------------ main --

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=REPO_ROOT / "binaries-esp32p4",
                    help="where bootloader/partition-table/app live "
                         "(default: binaries-esp32p4/, also accepts build/)")
    ap.add_argument("--out-dir", type=Path, default=REPO_ROOT / "binaries-esp32p4")
    ap.add_argument("--app-name", default="M5MonsterC5-Tab5.bin")
    ap.add_argument("--name", default="M5MonsterC5-Tab5-launcher.bin",
                    help="output filename dropped on the SD card")
    ap.add_argument("--version", default=None)
    ap.add_argument("--check", type=Path, metavar="BIN",
                    help="only validate an existing image, don't merge")
    args = ap.parse_args()

    try:
        if args.check:
            target = args.check
            if not target.exists():
                raise PackagingError(f"no such file: {target}")
        else:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            target = args.out_dir / args.name
            print(f"Merging Launcher image from {args.build_dir}")
            merge_image(args.build_dir, target, args.app_name)

        data = target.read_bytes()
        plan = plan_install(data)
        report(target, plan)

        if not args.check:
            version = args.version or project_version()
            digest = hashlib.sha256(data).hexdigest()
            (target.with_suffix(".bin.sha256")).write_text(f"{digest}  {target.name}\n")
            manifest = write_manifest(args.out_dir, target, plan, version)
            print(f"\n  -> {target}")
            print(f"  -> {target.with_suffix('.bin.sha256')}")
            print(f"  -> {manifest}")
            print(f"  version: {version}   sha256: {digest[:16]}...")
    except PackagingError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
