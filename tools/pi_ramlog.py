#!/usr/bin/env python3
"""Linux side of the RAM hand-off with the bare-metal FreeRTOS images.

Runs on the Pi as root. Linux reserves the regions with a no-map
reserved-memory overlay (see `install-overlay`), arms them before a tryboot
reboot, and reads them back once Linux is up again. The header layout matches
FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/src/ramlog.h.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import mmap
import os
import secrets
import struct
import subprocess
import sys
from pathlib import Path

REGIONS = (0x60000000, 0x30000000)
REGION_SIZE = 0x00100000
HEADER = struct.Struct("<IIQQIIIIIII12x")
HEADER_SIZE = 64
CAPACITY = REGION_SIZE - HEADER_SIZE
HOST_ARMED = 0x484F5354
FW_MAGIC = 0x52544F53
STATE_NAMES = {1: "running", 2: "complete"}
KIND_LOG = 1
KIND_PATTERN = 2

NONCE_FILE = Path("/var/tmp/freertos-ramlog-nonce.json")
OVERLAY_NAME = "freertos-ramlog"


def boot_dir() -> Path:
    for candidate in (Path("/boot/firmware"), Path("/boot")):
        if (candidate / "config.txt").exists():
            return candidate
    raise SystemExit("cannot find the boot partition")


def open_region(base: int) -> mmap.mmap:
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    try:
        return mmap.mmap(fd, REGION_SIZE, mmap.MAP_SHARED,
                         mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
    finally:
        os.close(fd)


def read_bytes(region: mmap.mmap, offset: int, length: int) -> bytes:
    """Copies out of /dev/mem with aligned 32-bit loads only. Slicing the mmap
    directly can issue unaligned or byte loads, which SIGBUS on this mapping
    (seen 9 Nov 2026 with a log length that was not a multiple of 4)."""
    start = offset & ~3
    end = (offset + length + 3) & ~3
    words = (ctypes.c_uint32 * ((end - start) // 4)).from_buffer(region, start)
    out = struct.pack(f"<{len(words)}I", *words)
    del words  # release the export so the caller can close the mmap
    return out[offset - start:offset - start + length]


def read_header(region: mmap.mmap) -> dict[str, int]:
    names = ("host_magic", "fw_magic", "host_nonce", "seen_nonce", "state",
             "length", "capacity", "checksum", "kind", "reports", "dropped")
    return dict(zip(names, HEADER.unpack(read_bytes(region, 0, HEADER_SIZE))))


def fnv1a(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def pattern(seed: int, words: int) -> bytes:
    state = seed
    out = bytearray(words * 4)
    for i in range(words):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        struct.pack_into("<I", out, i * 4, state)
    return bytes(out)


def region_reserved(base: int) -> bool:
    for line in Path("/proc/iomem").read_text().splitlines():
        span, _, name = line.strip().partition(" : ")
        start, end = (int(x, 16) for x in span.split("-"))
        if name == "System RAM" and start <= base <= end:
            return False
    return True


def cmd_install_overlay(_: argparse.Namespace) -> int:
    root = Path("/proc/device-tree/reserved-memory")
    addr_cells = struct.unpack(">I", (root / "#address-cells").read_bytes())[0]
    size_cells = struct.unpack(">I", (root / "#size-cells").read_bytes())[0]

    def cells(value: int, count: int) -> str:
        return " ".join(f"0x{(value >> (32 * i)) & 0xFFFFFFFF:x}" for i in reversed(range(count)))

    nodes = "\n".join(
        f"\t\t\tramlog@{base:x} {{\n"
        f"\t\t\t\treg = <{cells(base, addr_cells)} {cells(REGION_SIZE, size_cells)}>;\n"
        f"\t\t\t\tno-map;\n\t\t\t}};"
        for base in REGIONS
    )
    source = (
        "/dts-v1/;\n/plugin/;\n\n/ {\n\tcompatible = \"brcm,bcm2711\";\n\n"
        "\tfragment@0 {\n\t\ttarget-path = \"/reserved-memory\";\n"
        f"\t\t__overlay__ {{\n{nodes}\n\t\t}};\n\t}};\n}};\n"
    )
    boot = boot_dir()
    dts = Path("/var/tmp") / f"{OVERLAY_NAME}.dts"
    dts.write_text(source)
    subprocess.run(["dtc", "-q", "-@", "-I", "dts", "-O", "dtb", "-o",
                    str(boot / "overlays" / f"{OVERLAY_NAME}.dtbo"), str(dts)], check=True)

    config = boot / "config.txt"
    line = f"dtoverlay={OVERLAY_NAME}"
    if line not in config.read_text().splitlines():
        with config.open("a") as stream:
            stream.write(f"\n[all]\n# Reserved RAM for FreeRTOS benchmark hand-off\n{line}\n")
        print(f"added {line} to {config}; reboot once to apply")
    else:
        print(f"{line} already in {config}")
    return 0


def cmd_install_image(args: argparse.Namespace) -> int:
    boot = boot_dir()
    # The images carry an arm64 Image header and are linked at 0x200000, where
    # the firmware loads such a kernel (it ignores kernel_address; measured
    # 9 Nov 2026). Headerless images linked at 0x80000 never started.
    target = boot / "freertos8.img"
    target.write_bytes(Path(args.image).read_bytes())
    (boot / "tryboot.txt").write_text(
        "# One-shot boot into the FreeRTOS image (sudo reboot '0 tryboot').\n"
        "# Every other boot uses config.txt and Linux.\n"
        "[all]\narm_64bit=1\nkernel=freertos8.img\n"
        "# Fixed core clock: without an OS the firmware moved between 600 and\n"
        "# 1500 MHz mid-run (interference run 1, 9 Nov 2026). No over_voltage.\n"
        "arm_freq=1500\nforce_turbo=1\n"
        "disable_splash=1\n"
    )
    os.sync()
    print(f"installed {args.image} as {target}")
    return 0


def cmd_status(_: argparse.Namespace) -> int:
    ok = True
    for base in REGIONS:
        reserved = region_reserved(base)
        ok &= reserved
        print(f"0x{base:08x}: {'reserved' if reserved else 'NOT reserved (System RAM)'}")
    return 0 if ok else 1


def cmd_arm(_: argparse.Namespace) -> int:
    nonces = {}
    for base in REGIONS:
        if not region_reserved(base):
            raise SystemExit(f"0x{base:08x} is still System RAM; run install-overlay and reboot")
        nonce = secrets.randbits(64) | 1
        region = open_region(base)
        region[:] = bytes(REGION_SIZE)
        region[:HEADER_SIZE] = HEADER.pack(HOST_ARMED, 0, nonce, 0, 0, 0, 0, 0, 0, 0, 0)
        # No flush: msync on /dev/mem fails with EINVAL, and the no-map region
        # is mapped uncached, so the stores have already reached DRAM.
        region.close()
        nonces[f"0x{base:08x}"] = nonce
    NONCE_FILE.write_text(json.dumps(nonces))
    os.sync()
    print("armed " + ", ".join(nonces))
    return 0


def cmd_check_persist(_: argparse.Namespace) -> int:
    nonces = json.loads(NONCE_FILE.read_text())
    passed = 0
    for base in REGIONS:
        region = open_region(base)
        header = read_header(region)
        data = read_bytes(region, HEADER_SIZE, CAPACITY)
        region.close()
        label = f"0x{base:08x}"
        if header["fw_magic"] != FW_MAGIC or header["kind"] != KIND_PATTERN:
            print(f"{label}: FAIL, no FreeRTOS marker (image did not run, or RAM was cleared)")
            continue
        to_image = header["seen_nonce"] == nonces[label]
        seed = ((header["seen_nonce"] ^ (header["seen_nonce"] >> 32) ^ base) & 0xFFFFFFFF) | 1
        expected = pattern(seed, CAPACITY // 4)
        bad_words = sum(1 for i in range(0, CAPACITY, 4) if data[i:i + 4] != expected[i:i + 4])
        checksum_ok = fnv1a(data) == header["checksum"]
        verdict = "PASS" if to_image and bad_words == 0 and checksum_ok else "FAIL"
        passed += verdict == "PASS"
        print(f"{label}: {verdict}  linux->image nonce {'kept' if to_image else 'LOST'}, "
              f"image->linux {CAPACITY // 4 - bad_words}/{CAPACITY // 4} words intact, "
              f"checksum {'ok' if checksum_ok else 'BAD'}")
    print(f"ram_persistence={'PASS' if passed == len(REGIONS) else 'PARTIAL' if passed else 'FAIL'}")
    return 0 if passed else 1


def cmd_read(args: argparse.Namespace) -> int:
    nonces = json.loads(NONCE_FILE.read_text())
    base = REGIONS[0]
    region = open_region(base)
    header = read_header(region)
    data = read_bytes(region, HEADER_SIZE, min(header["length"], CAPACITY))
    region.close()
    if header["fw_magic"] != FW_MAGIC or header["kind"] != KIND_LOG:
        print("no FreeRTOS log in RAM (image did not run, or it is not a RAM_LOG=1 build)")
        return 1
    Path(args.out).write_bytes(data)
    state = STATE_NAMES.get(header["state"], str(header["state"]))
    checksum_ok = header["state"] != 2 or fnv1a(data) == header["checksum"]
    print(f"state={state} reports={header['reports']} bytes={len(data)} dropped={header['dropped']} "
          f"nonce={'ok' if header['seen_nonce'] == nonces[f'0x{base:08x}'] else 'MISMATCH'} "
          f"checksum={'ok' if checksum_ok else 'BAD'}")
    if state != "complete":
        print("log is incomplete: the watchdog reset the board before the run finished")
    return 0 if state == "complete" and checksum_ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("install-overlay", help="reserve the RAM regions (once, then reboot)")
    image = sub.add_parser("install-image", help="install an image for the next tryboot")
    image.add_argument("image")
    sub.add_parser("status", help="show whether the regions are reserved")
    sub.add_parser("arm", help="clear the regions and write fresh nonces")
    sub.add_parser("check-persist", help="verify a ram-persist-test run")
    read = sub.add_parser("read", help="save the log from a RAM_LOG=1 benchmark run")
    read.add_argument("--out", required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("run with sudo")
    handlers = {
        "install-overlay": cmd_install_overlay,
        "install-image": cmd_install_image,
        "status": cmd_status,
        "arm": cmd_arm,
        "check-persist": cmd_check_persist,
        "read": cmd_read,
    }
    return handlers[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
