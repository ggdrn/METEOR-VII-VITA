#!/usr/bin/env python3
"""
Turns ux0:data/ffvii-vita/profile.bin (written by source/utils/profiler.c)
into a ranking of the hottest functions of libjni_ff7.so.

Usage: scripts/profile-report.py profile.bin libjni_ff7.so [--top N] [--tid 0x...]
                                 [--from SECONDS] [--to SECONDS]

Records are {pc, lr, tid, ms} (v0.00.15+); --from/--to match the timestamps
of logs.txt. Older 12-byte profiles work with --legacy.

Symbols come from the .so's dynamic symbol table (arm-vita-eabi-nm must be on
PATH: `source scripts/env.sh`).
"""
import argparse
import bisect
import collections
import struct
import subprocess


def load_symbols(so_path):
    out = subprocess.run(["arm-vita-eabi-nm", "-D", "--defined-only", "-n", so_path],
                         capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in "tTwW":
            continue
        addrs.append(int(parts[0], 16) & ~1)
        names.append(parts[2])
    demangled = subprocess.run(["arm-vita-eabi-c++filt"], input="\n".join(names),
                               capture_output=True, text=True, check=True).stdout.splitlines()
    return addrs, demangled


def lookup(addrs, names, off):
    i = bisect.bisect_right(addrs, off & ~1) - 1
    return names[i] if i >= 0 else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("so")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--tid", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--from-frac", type=float, default=0.0,
                    help="skip the first fraction of the samples (they are in time order)")
    ap.add_argument("--from", dest="t_from", type=float, default=None, help="start time (s)")
    ap.add_argument("--to", dest="t_to", type=float, default=None, help="end time (s)")
    ap.add_argument("--legacy", action="store_true", help="12-byte records without timestamps")
    args = ap.parse_args()

    data = open(args.profile, "rb").read()
    if args.legacy:
        samples = [struct.unpack_from("<III", data, i) + (0,) for i in range(0, len(data) - 11, 12)]
    else:
        samples = [struct.unpack_from("<IIII", data, i) for i in range(0, len(data) - 15, 16)]
    samples = samples[int(len(samples) * args.from_frac):]
    if args.t_from is not None:
        samples = [s for s in samples if s[3] >= args.t_from * 1000]
    if args.t_to is not None:
        samples = [s for s in samples if s[3] <= args.t_to * 1000]
    addrs, names = load_symbols(args.so)

    by_tid = collections.Counter(s[2] for s in samples)
    print(f"{len(samples)} samples")
    for tid, n in by_tid.most_common():
        print(f"  thread 0x{tid:08x}: {n} ({100.0 * n / len(samples):.1f}%)")
    if args.tid is None and by_tid:
        args.tid = by_tid.most_common(1)[0][0]
    samples = [s for s in samples if s[2] == args.tid]
    total = len(samples)
    print(f"\nthread 0x{args.tid:08x}: {total} samples\n")
    if not total:
        return

    funcs = collections.Counter(lookup(addrs, names, s[0]) for s in samples)
    callers = collections.defaultdict(collections.Counter)
    for pc, lr, _, _ in samples:
        callers[lookup(addrs, names, pc)][lookup(addrs, names, lr)] += 1

    print(f"{'%':>6} {'samples':>8}  function (top callers by LR)")
    for name, n in funcs.most_common(args.top):
        top_callers = ", ".join(f"{c} {100.0 * k / n:.0f}%" for c, k in callers[name].most_common(2))
        print(f"{100.0 * n / total:6.2f} {n:8d}  {name}   <- {top_callers}")


if __name__ == "__main__":
    main()
