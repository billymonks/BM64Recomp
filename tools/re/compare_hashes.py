"""Compare two BM64_HASH_LOG files: per range, the first differing tick, how many ticks differ, and the last one.

Usage: python compare_hashes.py record_hashes.csv replay_hashes.csv
Ranges present in only one file are listed and skipped. Incomplete trailing rows (a log still being written) are ignored.
"""
import csv, sys


def load(path):
    with open(path, newline='') as f:
        rows = list(csv.reader(f))
    header = rows[0]
    return header, [r for r in rows[1:] if len(r) == len(header)]


def main(a, b):
    header_a, rows_a = load(a)
    header_b, rows_b = load(b)
    common = [c for c in header_a[3:] if c in header_b[3:]]
    only = [c for c in header_a[3:] + header_b[3:] if c not in common]
    if only:
        print(f'ranges in only one file (skipped): {", ".join(sorted(set(only)))}')
    n = min(len(rows_a), len(rows_b))
    stats = {}
    for i in range(n):
        ra, rb = rows_a[i], rows_b[i]
        for name in common:
            if ra[header_a.index(name)] != rb[header_b.index(name)]:
                s = stats.setdefault(name, [ra[:3], 0, None])
                s[1] += 1
                s[2] = ra[0]
    print(f'{n} ticks compared ({len(rows_a)} vs {len(rows_b)} rows)')
    if not stats:
        print('identical')
        return 0
    for name in common:
        if name in stats:
            (tick, frame, ovl), count, last = stats[name]
            print(f'{name:<8} first differs at tick {tick} (input frame {frame}, overlay id {ovl}); '
                  f'differs on {count} ticks, last {last}')
        else:
            print(f'{name:<8} identical')
    return 2


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
