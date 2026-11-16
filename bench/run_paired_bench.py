#!/usr/bin/env python3
"""Standalone Latin-square paired KVM / QEMU benchmark harness for reproducing EvoCore vs Vanilla Linux 7.3.4."""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import time
from pathlib import Path
from typing import Any, Dict, List


def run_once(
    backend: str,
    kernel_path: Path,
    initrd_path: Path,
    cpus: int,
    mem_mb: int,
    pin_cpus: str | None,
    timeout_s: int,
) -> Dict[str, Any]:
    if backend == "cloud-hypervisor":
        base_cmd = [
            "cloud-hypervisor",
            "--kernel", str(kernel_path),
            "--initramfs", str(initrd_path),
            "--cpus", f"boot={cpus}",
            "--memory", f"size={mem_mb}M",
            "--cmdline", "console=ttyS0 quiet panic=-1 rdinit=/init",
            "--serial", "tty",
            "--console", "off",
        ]
        if pin_cpus:
            cmd = ["taskset", "-c", pin_cpus] + base_cmd
        else:
            cmd = base_cmd
    elif backend == "qemu-tcg":
        cmd = [
            "qemu-system-x86_64",
            "-machine", "pc",
            "-accel", "tcg,thread=multi",
            "-smp", str(cpus),
            "-m", f"{mem_mb}M",
            "-kernel", str(kernel_path),
            "-initrd", str(initrd_path),
            "-append", "console=ttyS0 quiet panic=-1 rdinit=/init",
            "-nographic",
            "-no-reboot",
        ]
    else:
        raise ValueError(f"Unsupported backend: {backend}")

    t0 = time.monotonic()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    wall_s = round(time.monotonic() - t0, 3)
    out = proc.stdout + "\n" + proc.stderr
    m = re.search(r"RESULT_JSON:(\{.*\})", out)
    if not m:
        raise RuntimeError(f"Missing RESULT_JSON (rc={proc.returncode}): {out[-600:]}")
    metrics = json.loads(m.group(1))
    if metrics.get("sanity_rc") != 0 or metrics.get("kmsg_clean") != 1 or metrics.get("tainted") != 0:
        raise RuntimeError(f"Kernel sanity gate failed: {metrics}")
    return {"wall_s": wall_s, "metrics": metrics}


def main() -> int:
    parser = argparse.ArgumentParser(description="Run paired Latin-square Linux kernel benchmarks.")
    parser.add_argument("--backend", choices=["cloud-hypervisor", "qemu-tcg"], default="cloud-hypervisor")
    parser.add_argument("--vanilla-vmlinux", type=Path, required=True)
    parser.add_argument("--vanilla-initrd", type=Path, required=True)
    parser.add_argument("--evocore-vmlinux", type=Path, required=True)
    parser.add_argument("--evocore-initrd", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--mem-mb", type=int, default=1536)
    parser.add_argument("--pin-cpus", type=str, default=None, help="Optional host CPU list for taskset, e.g. 8-11")
    parser.add_argument("--cooldown-s", type=float, default=8.0)
    parser.add_argument("--timeout-s", type=int, default=120)
    parser.add_argument("--out", type=Path, default=Path("paired_benchmark_out.json"))
    args = parser.parse_args()

    configs = [
        ("vanilla", args.vanilla_vmlinux, args.vanilla_initrd),
        ("evocore", args.evocore_vmlinux, args.evocore_initrd),
    ]
    results: Dict[str, List[Dict[str, Any]]] = {"vanilla": [], "evocore": []}

    for r in range(args.rounds):
        order = configs[r % 2 :] + configs[: r % 2]
        print(f"=== Round {r + 1}/{args.rounds} order={[x[0] for x in order]} ===", flush=True)
        for name, vml, ird in order:
            time.sleep(args.cooldown_s)
            res = run_once(args.backend, vml, ird, args.cpus, args.mem_mb, args.pin_cpus, args.timeout_s)
            results[name].append(res)
            m = res["metrics"]
            print(
                f"  [{name:8s}] wall={res['wall_s']}s futex={m.get('futex_pingpong_us')}us "
                f"getppid={m.get('syscall_getppid_ns')}ns hackbench={m.get('hackbench_pipe_ms')}ms "
                f"post_kib={m.get('post_used_kib')}",
                flush=True,
            )

    summary = {}
    for name in ("vanilla", "evocore"):
        keys = [k for k in results[name][0]["metrics"] if isinstance(results[name][0]["metrics"][k], (int, float))]
        summary[name] = {k: round(statistics.median(r["metrics"][k] for r in results[name]), 4) for k in keys}
        summary[name]["wall_s"] = round(statistics.median(r["wall_s"] for r in results[name]), 3)

    payload = {"backend": args.backend, "rounds": args.rounds, "medians": summary, "raw": results}
    args.out.write_text(json.dumps(payload, indent=2))
    print("Saved paired results to", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
