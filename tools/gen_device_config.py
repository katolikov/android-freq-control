#!/usr/bin/env python3
"""Generate a per-SoC C++ header by probing an attached Android device.

Usage:

    tools/gen_device_config.py --adb-serial <SERIAL> -o config/soc/<soc>.h

The script asks the device for everything it needs (SoC name, CPU topology
and freq tables, GPU/MIF devfreq nodes) and emits a self-contained header
that exposes `freq_control::<soc>::kDeviceConfig`, consumed by
src/device_config.cc when built with `-DFREQ_CONTROL_SOC=<soc>`.

There is no intermediate JSON file. Re-run the script when the device
changes; the header is the source of truth.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

# Stable emission order for the five named modes. The right-hand column is
# the C++ enumerator in FrequencyMode that each mode binds to.
_KNOWN_MODES: list[tuple[str, str]] = [
    ("power_save", "kPowerSave"),
    ("balanced", "kBalanced"),
    ("performance", "kPerformance"),
    ("boost", "kBoost"),
    ("maximum", "kMaximum"),
]

# Number-of-policies -> cluster labels, ordered low max-freq to high. Samsung's
# canonical 4-cluster layout is little/mid/big/prime; common 3-cluster SoCs
# map little/big/prime; 2-cluster is little/big.
_CLUSTER_LABELS_BY_COUNT: dict[int, list[str]] = {
    1: ["little"],
    2: ["little", "big"],
    3: ["little", "big", "prime"],
    4: ["little", "mid", "big", "prime"],
}

# (min, max) percentile pair per mode. Indices into the rail's sorted
# available_frequencies list: 0 = lowest, 1 = highest. Picking by index
# guarantees the chosen value is one the kernel will accept.
_MODE_PERCENTILES: dict[str, tuple[float, float]] = {
    "power_save": (0.00, 0.40),
    "balanced": (0.00, 0.65),
    "performance": (0.45, 0.90),
    "boost": (0.65, 1.00),
    "maximum": (1.00, 1.00),
}

_KNOWN_CLUSTER_ENUMS: dict[str, str] = {
    "little": "kLittle",
    "mid": "kMid",
    "big": "kBig",
    "prime": "kPrime",
}

_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


# -------------------------- adb helpers --------------------------------------


def _run_adb(serial: str, adb: str, shell_cmd: str) -> str:
    """Run `adb -s SERIAL shell <cmd>`; return stdout with CRs stripped."""
    args = [adb, "-s", serial, "shell", shell_cmd]
    proc = subprocess.run(args, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise SystemExit(
            f"adb command failed: {' '.join(args)}\n  stderr: {proc.stderr.strip()}"
        )
    return proc.stdout.replace("\r", "")


def _adb_cat(serial: str, adb: str, path: str) -> str:
    """cat a sysfs file; return empty string on read failure (e.g. perms)."""
    args = [adb, "-s", serial, "shell", f"cat {path} 2>/dev/null"]
    proc = subprocess.run(args, capture_output=True, text=True, check=False)
    return "" if proc.returncode != 0 else proc.stdout.replace("\r", "").strip()


def _adb_exists(serial: str, adb: str, path: str) -> bool:
    return _run_adb(serial, adb, f"[ -e {path} ] && echo y || echo n").strip() == "y"


# -------------------------- name + freq utilities ----------------------------


def _to_pascal(name: str) -> str:
    return "".join(p.capitalize() for p in re.split(r"[_\-]+", name))


def _sanitise_soc_name(raw: str) -> str:
    """Coerce a vendor property like 'Exynos 2500' into a C identifier."""
    cleaned = re.sub(r"[^A-Za-z0-9_]+", "_", raw.strip().lower()).strip("_")
    if not cleaned:
        raise SystemExit("device did not report any usable SoC name")
    if cleaned[0].isdigit():
        cleaned = "soc_" + cleaned
    if not _IDENT_RE.match(cleaned):
        raise SystemExit(f"derived SoC name '{cleaned}' is not a C identifier")
    return cleaned


def _pick_freq_pair(sorted_freqs: list[int], mode: str) -> tuple[int, int]:
    n = len(sorted_freqs)
    if n == 0:
        raise SystemExit(f"empty freq list while picking mode '{mode}'")
    if n == 1:
        return sorted_freqs[0], sorted_freqs[0]
    lo_pct, hi_pct = _MODE_PERCENTILES[mode]
    lo_idx = max(0, min(n - 1, round(lo_pct * (n - 1))))
    hi_idx = max(0, min(n - 1, round(hi_pct * (n - 1))))
    if hi_idx < lo_idx:
        hi_idx = lo_idx
    return sorted_freqs[lo_idx], sorted_freqs[hi_idx]


# -------------------------- device probing -----------------------------------


def _probe_cpu_policies(serial: str, adb: str) -> list[dict[str, Any]]:
    ls = _run_adb(serial, adb, "ls /sys/devices/system/cpu/cpufreq/")
    policy_dirs = sorted(
        (p for p in ls.split() if re.fullmatch(r"policy\d+", p)),
        key=lambda p: int(p[len("policy"):]),
    )
    if not policy_dirs:
        raise SystemExit("no /sys/devices/system/cpu/cpufreq/policy* found")
    policies: list[dict[str, Any]] = []
    for pname in policy_dirs:
        base = f"/sys/devices/system/cpu/cpufreq/{pname}"
        related = _adb_cat(serial, adb, f"{base}/related_cpus").split()
        freqs_raw = _adb_cat(serial, adb, f"{base}/scaling_available_frequencies").split()
        if not related or not freqs_raw:
            print(
                f"warning: skipping {pname} (related_cpus or freq table unreadable)",
                file=sys.stderr,
            )
            continue
        policies.append(
            {
                "policy_name": pname,
                "cpu_ids": sorted(int(x) for x in related),
                "freqs": sorted({int(x) for x in freqs_raw}),
            }
        )
    if not policies:
        raise SystemExit("no readable cpufreq policies on this device")
    return policies


def _probe_devfreq_node(
    serial: str, adb: str, node: str
) -> tuple[dict[str, str], list[int]] | None:
    """Pick min/max sysfs paths and read the freq table for a devfreq node.

    Prefers Samsung's `scaling_devfreq_{min,max}` (system-writable) when
    present, falls back to the standard `min_freq`/`max_freq` pair. Returns
    None if the freq table is unreadable.
    """
    base = f"/sys/class/devfreq/{node}"
    freqs_raw = _adb_cat(serial, adb, f"{base}/available_frequencies").split()
    if not freqs_raw:
        return None
    if _adb_exists(serial, adb, f"{base}/scaling_devfreq_min"):
        paths = {"min": f"{base}/scaling_devfreq_min", "max": f"{base}/scaling_devfreq_max"}
    else:
        paths = {"min": f"{base}/min_freq", "max": f"{base}/max_freq"}
    return paths, sorted({int(x) for x in freqs_raw})


def probe_device(serial: str, adb: str) -> dict[str, Any]:
    if shutil.which(adb) is None:
        raise SystemExit(f"adb binary not found: {adb}")

    soc_raw = _run_adb(serial, adb, "getprop ro.soc.model").strip()
    if not soc_raw:
        soc_raw = _run_adb(serial, adb, "getprop ro.hardware").strip()
    soc = _sanitise_soc_name(soc_raw)
    print(f"probe: soc='{soc}' (ro.soc.model={soc_raw!r})", file=sys.stderr)

    policies = _probe_cpu_policies(serial, adb)
    if len(policies) not in _CLUSTER_LABELS_BY_COUNT:
        raise SystemExit(
            f"unsupported cluster count: {len(policies)} (handler covers 1..4)"
        )
    policies.sort(key=lambda p: p["freqs"][-1])
    for p, label in zip(policies, _CLUSTER_LABELS_BY_COUNT[len(policies)], strict=True):
        p["cluster"] = label

    cpu_clusters: dict[str, list[int]] = {p["cluster"]: p["cpu_ids"] for p in policies}
    rails: dict[str, dict[str, str]] = {}
    modes: dict[str, dict[str, tuple[int, int]]] = {m: {} for m, _ in _KNOWN_MODES}

    for p in policies:
        rail = f"cpu_{p['cluster']}"
        base = f"/sys/devices/system/cpu/cpufreq/{p['policy_name']}"
        rails[rail] = {"min": f"{base}/scaling_min_freq", "max": f"{base}/scaling_max_freq"}
        for mode in modes:
            modes[mode][rail] = _pick_freq_pair(p["freqs"], mode)

    devfreq_ls = _run_adb(serial, adb, "ls /sys/class/devfreq/ 2>/dev/null").split()
    extra = (("gpu", "sgpu"), ("mif", "mif"))
    for rail_name, needle in extra:
        node = next((d for d in devfreq_ls if needle in d.lower()), None)
        if node is None:
            print(f"probe: no {rail_name} devfreq node found, skipping", file=sys.stderr)
            continue
        probed = _probe_devfreq_node(serial, adb, node)
        if probed is None:
            print(
                f"probe: {rail_name} node '{node}' has no readable freq table, skipping",
                file=sys.stderr,
            )
            continue
        paths, freqs = probed
        rails[rail_name] = paths
        for mode in modes:
            modes[mode][rail_name] = _pick_freq_pair(freqs, mode)
        print(f"probe: {rail_name}='{node}' ({len(freqs)} freqs)", file=sys.stderr)

    return {
        "soc": soc,
        "cpu_clusters": cpu_clusters,
        "rails": rails,
        "modes": modes,
    }


# -------------------------- header emission ----------------------------------


def _rail_constant_name(rail: str, side: str) -> str:
    return f"k{_to_pascal(rail)}{side}"


def _emit_rail_paths(out: list[str], rails: dict[str, dict[str, str]]) -> None:
    out.append("// Rail sysfs paths.")
    for rail in sorted(rails):
        entry = rails[rail]
        if entry.get("min"):
            out.append(f'inline constexpr char {_rail_constant_name(rail, "Min")}[] = "{entry["min"]}";')
        if entry.get("max"):
            out.append(f'inline constexpr char {_rail_constant_name(rail, "Max")}[] = "{entry["max"]}";')
    out.append("")


def _emit_targets(
    out: list[str],
    array_name: str,
    rails: dict[str, dict[str, str]],
    profile_freqs: dict[str, tuple[int, int]],
) -> None:
    out.append(f"inline constexpr RailTarget {array_name}[] = {{")
    for rail, (lo, hi) in profile_freqs.items():
        min_const = _rail_constant_name(rail, "Min") if rails[rail].get("min") else "nullptr"
        max_const = _rail_constant_name(rail, "Max") if rails[rail].get("max") else "nullptr"
        out.append(f"    {{{min_const}, {max_const}, {lo}ULL, {hi}ULL}},")
    out.append("};")
    out.append("")


def _emit_cpu_clusters(
    out: list[str], clusters: dict[str, list[int]]
) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    for label, enum_name in _KNOWN_CLUSTER_ENUMS.items():
        if label not in clusters:
            continue
        ids = clusters[label]
        array_name = f"kCluster{_to_pascal(label)}Cpus"
        out.append(f"inline constexpr uint32_t {array_name}[] = {{")
        out.append("    " + ", ".join(f"{i}u" for i in ids) + ",")
        out.append("};")
        entries.append((enum_name, label, array_name))
    if entries:
        out.append("")
    return entries


def build_header(spec: dict[str, Any]) -> str:
    soc = spec["soc"]
    if not _IDENT_RE.match(soc):
        raise SystemExit(f"soc '{soc}' is not a C identifier")

    out: list[str] = []
    guard = f"FREQ_CONTROL_CONFIG_SOC_{soc.upper()}_H_"
    out += [
        "// AUTO-GENERATED by tools/gen_device_config.py - DO NOT EDIT BY HAND.",
        "// Regenerate with:",
        f"//   tools/gen_device_config.py --adb-serial <SERIAL> -o {soc}.h",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <cstdint>",
        "#include <iterator>",
        "",
        '#include "freq_control/device_config.h"',
        "",
        "namespace freq_control {",
        f"namespace {soc} {{",
        "",
    ]

    _emit_rail_paths(out, spec["rails"])

    mode_entries: list[tuple[str, str, str]] = []  # (enum_value, label, array_name)
    for label, enum_value in _KNOWN_MODES:
        if label not in spec["modes"]:
            continue
        array_name = f"k{_to_pascal(label)}Targets"
        out.append(f"// FrequencyMode::{enum_value} ({label})")
        _emit_targets(out, array_name, spec["rails"], spec["modes"][label])
        mode_entries.append((enum_value, label, array_name))

    if mode_entries:
        out.append("inline constexpr ModeProfile kModes[] = {")
        for enum_value, label, array_name in mode_entries:
            out.append(
                f'    {{FrequencyMode::{enum_value}, "{label}", {array_name}, '
                f"std::size({array_name})}},"
            )
        out.append("};")
        out.append("")

    cluster_entries = _emit_cpu_clusters(out, spec["cpu_clusters"])
    if cluster_entries:
        out.append("inline constexpr CpuClusterMap kClusters[] = {")
        for enum_value, label, array_name in cluster_entries:
            out.append(
                f'    {{CpuCluster::{enum_value}, "{label}", {array_name}, '
                f"std::size({array_name})}},"
            )
        out.append("};")
        out.append("")

    out.append("inline constexpr DeviceConfig kDeviceConfig = {")
    out.append(f'    "{soc}",')
    out.append("    kModes, std::size(kModes)," if mode_entries else "    nullptr, 0,")
    out.append("    nullptr, 0,  // custom profiles (none by default)")
    out.append(
        "    kClusters, std::size(kClusters)," if cluster_entries else "    nullptr, 0,"
    )
    out.append("};")
    out.append("")
    out.append(f"}}  // namespace {soc}")
    out.append("}  // namespace freq_control")
    out.append("")
    out.append(f"#endif  // {guard}")
    out.append("")

    return "\n".join(out)


# -------------------------- entry point --------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--adb-serial",
        required=True,
        help="ADB serial of an attached device (see `adb devices`).",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="Header path to write (e.g. config/soc/<soc>.h).",
    )
    parser.add_argument(
        "--adb",
        default="adb",
        help="Path to the adb binary (default: search PATH for 'adb').",
    )
    args = parser.parse_args()

    spec = probe_device(args.adb_serial, args.adb)
    header = build_header(spec)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header)
    print(f"wrote {args.output} ({len(header.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
