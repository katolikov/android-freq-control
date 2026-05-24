#!/usr/bin/env python3
"""Apply a FreqControl mode on a device and observe before/during/after clocks.

Usage:
    run.py -s <SERIAL> -m <MODE>           # default hold duration: 5 s
    run.py -s <SERIAL> -m boost -d 10      # hold 10 s
    run.py -s <SERIAL> -m 1001             # by custom id

What the script does:
    1. Pushes build/freq_ctl to /data/local/tmp/freq_ctl (unless --no-push).
    2. Asks the on-device binary which sysfs paths the chosen mode touches
       (parses `freq_ctl list`).
    3. Reads each path -> BEFORE snapshot.
    4. Spawns `freq_ctl cycle <mode> <duration>` in the background (the
       binary calls SetClocks, sleeps, then UnsetClocks).
    5. After ~1 s reads each path again -> DURING snapshot.
    6. Waits for the cycle to finish, reads paths once more -> AFTER snapshot.
    7. Diffs AFTER against BEFORE; reports any rail that didn't restore.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
LOCAL_BIN_DEFAULT = REPO_ROOT / "build" / "freq_ctl"
REMOTE_BIN = "/data/local/tmp/freq_ctl"

_RAIL_LINE_RE = re.compile(r"min\[([^\]]+)\]\s*=\s*\d+\s*kHz,\s*max\[([^\]]+)\]")
_MODE_HEADER_RE = re.compile(r"^  (\S+) \(mode=\d+, \d+ rails\)")
_CUSTOM_HEADER_RE = re.compile(r"^  id=(\d+) name=(\S+) \(\d+ rails\)")


def _adb(serial: str, *args: str, capture: bool = True, check: bool = True) -> str:
    cmd = ["adb", "-s", serial, *args]
    proc = subprocess.run(cmd, capture_output=capture, text=True, check=False)
    if check and proc.returncode != 0:
        raise SystemExit(
            f"adb command failed: {' '.join(cmd)}\n  stderr: {proc.stderr.strip()}"
        )
    return proc.stdout.replace("\r", "") if capture else ""


def _shell(serial: str, cmd: str) -> str:
    return _adb(serial, "shell", cmd)


def _push_binary(serial: str, local_bin: Path) -> None:
    if not local_bin.exists():
        raise SystemExit(
            f"binary not found: {local_bin}\n"
            f"  build it first, e.g.:\n"
            f"    cmake -S . -B build \\\n"
            f"        -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \\\n"
            f"        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \\\n"
            f"        -DFREQ_CONTROL_SOC=<soc>\n"
            f"    cmake --build build"
        )
    _adb(serial, "push", str(local_bin), REMOTE_BIN)
    _shell(serial, f"chmod +x {REMOTE_BIN}")


def _freq_ctl(serial: str, device: str | None, *args: str) -> str:
    prefix = f"{REMOTE_BIN} --device {device} " if device else f"{REMOTE_BIN} "
    return _shell(serial, prefix + " ".join(args))


def _rails_for_mode(
    serial: str, device: str | None, mode: str
) -> list[tuple[str, str]]:
    """Return [(min_path, max_path), ...] for the requested mode or custom id."""
    out = _freq_ctl(serial, device, "list")
    rails: list[tuple[str, str]] = []
    in_target = False
    for line in out.splitlines():
        mh = _MODE_HEADER_RE.match(line)
        ch = _CUSTOM_HEADER_RE.match(line)
        if mh is not None:
            in_target = mh.group(1) == mode
            continue
        if ch is not None:
            in_target = ch.group(1) == mode or ch.group(2) == mode
            continue
        if in_target:
            m = _RAIL_LINE_RE.search(line)
            if m:
                rails.append((m.group(1), m.group(2)))
    if not rails:
        raise SystemExit(
            f"mode '{mode}' has no rails (not declared in `freq_ctl list`)"
        )
    return rails


def _read_snapshot(serial: str, rails: list[tuple[str, str]]) -> list[dict[str, str]]:
    paths_cmd = ";".join(f"cat {p}" for r in rails for p in r)
    out = _shell(serial, paths_cmd).splitlines()
    if len(out) != 2 * len(rails):
        raise SystemExit(
            f"expected {2 * len(rails)} values from `cat`, got {len(out)}:\n{out}"
        )
    snap = []
    for i, (min_p, max_p) in enumerate(rails):
        snap.append(
            {
                "min_path": min_p,
                "min_val": out[2 * i].strip(),
                "max_path": max_p,
                "max_val": out[2 * i + 1].strip(),
            }
        )
    return snap


def _rail_label(min_path: str) -> str:
    parts = min_path.rstrip("/").split("/")
    return parts[-2] if len(parts) >= 2 else min_path


def _print_snapshot(label: str, snap: list[dict[str, str]]) -> None:
    print(f"\n=== {label} ===")
    for row in snap:
        print(
            f"  {_rail_label(row['min_path']):<26}  "
            f"min={int(row['min_val']):>9} kHz  "
            f"max={int(row['max_val']):>9} kHz"
        )


def _diff_snapshots(
    before: list[dict[str, str]], after: list[dict[str, str]]
) -> list[tuple[str, str, str, str]]:
    mismatches: list[tuple[str, str, str, str]] = []
    for b, a in zip(before, after, strict=True):
        if b["min_val"] != a["min_val"]:
            mismatches.append(("min", b["min_path"], b["min_val"], a["min_val"]))
        if b["max_val"] != a["max_val"]:
            mismatches.append(("max", b["max_path"], b["max_val"], a["max_val"]))
    return mismatches


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("-s", "--serial", required=True, help="adb serial")
    parser.add_argument(
        "-m",
        "--mode",
        required=True,
        help="mode name (power_save/balanced/performance/boost/maximum) "
        "or a custom id (e.g. 1001) or custom name",
    )
    parser.add_argument(
        "-D",
        "--device",
        default=None,
        help="SoC name to operate on (default: freq_ctl's first registered device)",
    )
    parser.add_argument(
        "-d",
        "--duration",
        type=int,
        default=5,
        help="seconds to hold the mode applied (default: 5)",
    )
    parser.add_argument(
        "--no-push",
        action="store_true",
        help="skip pushing the binary (assume already at /data/local/tmp/freq_ctl)",
    )
    parser.add_argument(
        "--bin",
        type=Path,
        default=LOCAL_BIN_DEFAULT,
        help="local path to freq_ctl (default: build/freq_ctl)",
    )
    args = parser.parse_args()

    if not args.no_push:
        _push_binary(args.serial, args.bin)

    rails = _rails_for_mode(args.serial, args.device, args.mode)
    print(
        f"mode '{args.mode}' touches {len(rails)} rail(s)"
        + (f" on device '{args.device}'" if args.device else "")
    )

    before = _read_snapshot(args.serial, rails)
    _print_snapshot("BEFORE", before)

    print(
        f"\napplying via `freq_ctl cycle {args.mode} {args.duration}` "
        f"(SetClocks -> sleep {args.duration}s -> UnsetClocks) ..."
    )
    cycle_cmd = (
        f"{REMOTE_BIN} --device {args.device} cycle {args.mode} {args.duration}"
        if args.device
        else f"{REMOTE_BIN} cycle {args.mode} {args.duration}"
    )
    cycle_proc = subprocess.Popen(
        ["adb", "-s", args.serial, "shell", cycle_cmd],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Give SetClocks time to land before sampling DURING.
    settle = min(1.5, max(0.5, args.duration / 4))
    time.sleep(settle)
    during = _read_snapshot(args.serial, rails)
    _print_snapshot(f"DURING (~{settle:.1f}s in)", during)

    try:
        stdout, stderr = cycle_proc.communicate(timeout=args.duration + 15)
    except subprocess.TimeoutExpired:
        cycle_proc.kill()
        stdout, stderr = cycle_proc.communicate()
        print("WARN: cycle process timed out, killed", file=sys.stderr)

    if cycle_proc.returncode != 0:
        print(
            f"\nWARN: freq_ctl cycle exited {cycle_proc.returncode}\n"
            f"  stdout: {stdout.strip()}\n"
            f"  stderr: {stderr.strip()}",
            file=sys.stderr,
        )

    after = _read_snapshot(args.serial, rails)
    _print_snapshot("AFTER", after)

    mismatches = _diff_snapshots(before, after)
    print()
    if not mismatches:
        print("PASS: AFTER matches BEFORE -- UnsetClocks restored correctly.")
        return 0
    print(f"FAIL: {len(mismatches)} value(s) did not restore:")
    for side, path, b, a in mismatches:
        print(f"  ({side}) {path}\n        before={b}  after={a}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
