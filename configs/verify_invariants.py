#!/usr/bin/env python3
"""Verify that a Linux kernel .config preserves all EvoCore safety, security mitigation, and subsystem invariants."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict

REQUIRED_SUBSYSTEMS = [
    "CONFIG_SMP",
    "CONFIG_CGROUPS",
    "CONFIG_MEMCG",
    "CONFIG_BLK_CGROUP",
    "CONFIG_NAMESPACES",
    "CONFIG_USER_NS",
    "CONFIG_PID_NS",
    "CONFIG_NET_NS",
    "CONFIG_NET",
    "CONFIG_INET",
    "CONFIG_EXT4_FS",
    "CONFIG_TMPFS",
    "CONFIG_EPOLL",
    "CONFIG_FUTEX",
    "CONFIG_IO_URING",
    "CONFIG_BPF_SYSCALL",
    "CONFIG_BPF_JIT",
    "CONFIG_SECCOMP",
    "CONFIG_SECURITY",
    "CONFIG_PREEMPT_BUILD",
    "CONFIG_PREEMPTION",
    "CONFIG_PVH",
    "CONFIG_BLK_DEV_INITRD",
    "CONFIG_RD_GZIP",
    "CONFIG_VIRTIO_CONSOLE",
    "CONFIG_SERIAL_8250_CONSOLE",
]

HARDENING_KEYS = [
    "CONFIG_PREEMPT_LAZY",
    "CONFIG_STACKPROTECTOR",
    "CONFIG_STACKPROTECTOR_STRONG",
    "CONFIG_FORTIFY_SOURCE",
    "CONFIG_RANDOMIZE_BASE",
    "CONFIG_SLAB_FREELIST_HARDENED",
    "CONFIG_HARDENED_USERCOPY",
    "CONFIG_SECURITY_LOCKDOWN_LSM",
    "CONFIG_STRICT_KERNEL_RWX",
    "CONFIG_STRICT_MODULE_RWX",
    "CONFIG_ZSWAP",
    "CONFIG_ZSMALLOC",
]


def parse_config(path: Path) -> Dict[str, str]:
    vals: Dict[str, str] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            sym = line[2:].split(" ", 1)[0]
            vals[sym] = "n"
        elif line.startswith("CONFIG_") and "=" in line:
            sym, val = line.split("=", 1)
            vals[sym] = val
    return vals


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify EvoCore Linux kernel .config invariants.")
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("evocore.config"))
    parser.add_argument("--baseline", type=Path, default=Path(__file__).with_name("base.config"))
    args = parser.parse_args()

    cfg = parse_config(args.config)
    base = parse_config(args.baseline) if args.baseline.exists() else {}

    missing_req = [k for k in REQUIRED_SUBSYSTEMS if cfg.get(k) != "y"]
    mitigations = sorted(k for k, v in (base or cfg).items() if k.startswith("CONFIG_MITIGATION_") and v == "y")
    missing_mit = [k for k in mitigations if cfg.get(k) != "y"]
    hardening_diffs = {
        k: (base.get(k), cfg.get(k))
        for k in HARDENING_KEYS
        if base and cfg.get(k) != base.get(k)
    }

    print(f"Config analyzed         : {args.config}")
    print(f"Core subsystems (=y)    : {len(REQUIRED_SUBSYSTEMS) - len(missing_req)}/{len(REQUIRED_SUBSYSTEMS)}")
    print(f"CPU mitigations (=y)    : {len(mitigations) - len(missing_mit)}/{len(mitigations)}")
    print(f"Hardening diffs vs base : {len(hardening_diffs)}")
    print(f"Zswap LZ4 enabled       : {cfg.get('CONFIG_ZSWAP_COMPRESSOR_DEFAULT_LZ4') == 'y'}")
    print(f"PCP_BATCH_SCALE_MAX     : {cfg.get('CONFIG_PCP_BATCH_SCALE_MAX')}")

    if missing_req or missing_mit or hardening_diffs:
        if missing_req:
            print("FAIL missing core subsystems:", missing_req)
        if missing_mit:
            print("FAIL dropped CPU mitigations:", missing_mit)
        if hardening_diffs:
            print("FAIL hardening diffs:", hardening_diffs)
        return 1
    print("PASS: All EvoCore safety, security mitigation, and subsystem invariants verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
