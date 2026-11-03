#!/usr/bin/env python3
"""Summarize fighter timing windows in a returned frontend.txt or boot directory.

Window percentiles are never combined into a claimed whole-run percentile.
The original log and perf.csv remain necessary for chronology and CPU analysis.
"""
import argparse
import collections
import pathlib
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=pathlib.Path)
    args = parser.parse_args()
    path = args.log / 'frontend.txt' if args.log.is_dir() else args.log
    text = path.read_text(errors='replace')
    rows = collections.defaultdict(list)
    pattern = r'fight: scene (\d+) (logic|render|audio) us avg/p95/p99/max (\d+)/(\d+)/(\d+)/(\d+), (\d+) runs'
    for match in re.finditer(pattern, text):
        scene, part, avg, p95, p99, peak, runs = match.groups()
        rows[int(scene), part].append(tuple(map(int, (avg, p95, p99, peak, runs))))
    if not rows:
        parser.error(f'{path}: no fighter timing windows found')
    print(f'Log: {path}')
    print(f'10-minute completion markers: {text.count("fight: benchmark complete:")}')
    print('Scene extra sprites: 0=0, 1=8, 2=24, 3=48 (128x160 alpha/add)')
    print('scene part    runs   avg_ms  max_window_p99_ms  max_ms')
    for (scene, part), samples in sorted(rows.items()):
        runs = sum(row[4] for row in samples)
        avg = sum(row[0] * row[4] for row in samples) / runs / 1000
        p99 = max(row[2] for row in samples) / 1000
        peak = max(row[3] for row in samples) / 1000
        print(f'{scene:5} {part:6} {runs:6} {avg:8.3f} {p99:18.3f} {peak:7.3f}')
    rss = [int(n) for n in re.findall(r'frontend peak RSS (\d+) KiB', text)]
    if rss:
        print(f'Frontend peak RSS: {max(rss)} KiB (whole process)')
    for line in text.splitlines():
        if any(key in line for key in ('fight: pacing', 'perf summary: adaptive all:',
                                        'perf summary: max all:', 'benchmark complete:')):
            print(line)
    print('Use perf.csv and perf lines for rotate/output, CPU, temperature and underruns.')
    print('Host/qemu timings do not predict RG351P performance.')


if __name__ == '__main__':
    main()
