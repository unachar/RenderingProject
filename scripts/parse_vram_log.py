#!/usr/bin/env python3
"""Summarize D3D12MA statistics lines from a graphics log.

The allocator logs periodically (MemoryAllocator::LogStatistics):
    VRAM: D3D12MA heaps 12, heap 512.00 MiB, allocated 480.25 MiB
    (waste 6.2%), allocations 340

Usage: parse_vram_log.py <logfile> [<logfile>...]
Prints per-file sample count and first/last/peak heap and
allocated figures - the A/B readout for the D3D12MA evaluation
(compare a run with the flag OFF, which logs nothing, against ON).
"""
import re
import sys

PATTERN = re.compile(
    r"VRAM: D3D12MA heaps (\d+), heap ([\d.]+) MiB, allocated ([\d.]+) MiB "
    r"\(waste ([\d.]+)%\), allocations (\d+)")


def summarize(path):
    samples = []
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = PATTERN.search(line)
            if match:
                heaps, heap_mb, alloc_mb, waste, allocs = match.groups()
                samples.append((int(heaps), float(heap_mb), float(alloc_mb),
                                float(waste), int(allocs)))
    if not samples:
        print(f"{path}: no D3D12MA statistics lines")
        return
    peak_heap = max(s[1] for s in samples)
    peak_alloc = max(s[2] for s in samples)
    last = samples[-1]
    print(f"{path}: {len(samples)} samples, peak heap {peak_heap:.2f} MiB, "
          f"peak allocated {peak_alloc:.2f} MiB, last: heaps {last[0]}, "
          f"heap {last[1]:.2f} MiB, allocated {last[2]:.2f} MiB "
          f"(waste {last[3]:.1f}%), allocations {last[4]}")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        summarize(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
