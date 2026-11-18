"""Compare two BM64_HASH_LOG files and report the first tick where they differ, per range.

Usage: python compare_hashes.py record_hashes.csv replay_hashes.csv
"""
import csv, sys


def load(path):
    with open(path, newline='') as f:
        rows = list(csv.reader(f))
    return rows[0], rows[1:]


def main(a, b):
    header_a, rows_a = load(a)
    header_b, rows_b = load(b)
    if header_a != header_b:
        print(f'different columns:\n  {header_a}\n  {header_b}')
        return 1
    n = min(len(rows_a), len(rows_b))
    first = {}
    for i in range(n):
        ra, rb = rows_a[i], rows_b[i]
        for col in range(3, len(header_a)):
            name = header_a[col]
            if name not in first and ra[col] != rb[col]:
                first[name] = (ra[0], ra[1], ra[2])
        if 'total' in first and len(first) == len(header_a) - 3:
            break
    print(f'{n} ticks compared ({len(rows_a)} vs {len(rows_b)} rows)')
    if not first:
        print('identical')
        return 0
    for name, (tick, frame, ovl) in sorted(first.items(), key=lambda kv: int(kv[1][0])):
        print(f'{name:<8} first differs at tick {tick} (input frame {frame}, overlay id {ovl})')
    return 2


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
