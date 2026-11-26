"""Compare two BM64_HASH_LOG files: per range, the first differing tick, how many ticks differ, and the last one.

Usage: python compare_hashes.py [--by-tick] a_hashes.csv b_hashes.csv
Ranges present in only one file are listed and skipped. Incomplete trailing rows (a log still being written) are ignored.

Without --by-tick, rows are compared in order (two replays, or a recording and its replay).
With --by-tick, each file keeps the last row logged for each tick and ticks present in both are compared.
Use it for the two peers of an online session: a peer logs every step it simulates, so a tick it
resimulated after a rollback appears several times, and only the last one is final.
"""
import csv, sys


def load(path):
    with open(path, newline='') as f:
        rows = list(csv.reader(f))
    header = rows[0]
    return header, [r for r in rows[1:] if len(r) == len(header)]


def last_per_tick(rows):
    by_tick = {}
    for r in rows:
        by_tick[int(r[0])] = r
    return [by_tick[t] for t in sorted(by_tick)]


def main(a, b, by_tick=False):
    header_a, rows_a = load(a)
    header_b, rows_b = load(b)
    if by_tick:
        rows_a = last_per_tick(rows_a)
        rows_b = last_per_tick(rows_b)
        ticks = {r[0] for r in rows_a} & {r[0] for r in rows_b}
        rows_a = [r for r in rows_a if r[0] in ticks]
        rows_b = [r for r in rows_b if r[0] in ticks]
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
    args = sys.argv[1:]
    by_tick = '--by-tick' in args
    args = [x for x in args if x != '--by-tick']
    if len(args) != 2:
        raise SystemExit(__doc__)
    sys.exit(main(args[0], args[1], by_tick))
