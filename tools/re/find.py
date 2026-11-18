"""Search function and data symbol names (regex, case-insensitive) or look up what lives at an address.

Usage:
  python find.py HuPrc                     names matching a regex
  python find.py 0x802AC894                symbol containing / at an address (main, boot and all overlays)
  python find.py --data g_Random           data symbols only
  python find.py --func --ovl 4.10 Sleep   functions only, in overlay 4.10 (bank 4, sub-overlay 10)
"""
import re, sys
from syms import FUNC_SECS, DATA_SECS


def _ovl_match(secname, flt):
    short = secname.replace('.ovl_', '').replace('_subovl_', '.')
    return flt in secname or flt == short or flt.replace('_', '.') == short


def main(argv):
    want_func = want_data = True
    ovl_filter = None
    while argv and argv[0].startswith('--'):
        flag = argv.pop(0)
        if flag == '--func':
            want_data = False
        elif flag == '--data':
            want_func = False
        elif flag == '--ovl':
            ovl_filter = argv.pop(0)
    if not argv:
        raise SystemExit(__doc__)
    q = argv[0]
    if re.fullmatch(r'0x[0-9A-Fa-f]+', q):
        addr = int(q, 16)
        for s in FUNC_SECS:
            for n, v, sz in s['funcs']:
                if v <= addr < v + sz:
                    print(f'func {s["name"]:<20} {v:08X} size {sz:<6X} {n}  (+0x{addr - v:X})')
        best = {}
        for s in DATA_SECS:
            cands = [(v, n) for n, v in s['syms'] if v <= addr and not n.endswith(('_START', '_END', '_VRAM', '_VRAM_END'))]
            if cands and s['vram'] <= addr < s['vram'] + max(s['size'], 1):
                v, n = max(cands)
                best[s['name']] = (v, n)
        for sec, (v, n) in best.items():
            print(f'data {sec:<20} {v:08X} {n}  (+0x{addr - v:X})')
        return
    rx = re.compile(q, re.I)
    if want_func:
        for s in FUNC_SECS:
            if ovl_filter and not _ovl_match(s['name'], ovl_filter):
                continue
            for n, v, sz in s['funcs']:
                if rx.search(n) and not n.endswith('__secure_call'):
                    print(f'func {s["name"]:<20} {v:08X} size {sz:<6X} {n}')
    if want_data:
        seen = set()
        for s in DATA_SECS:
            if ovl_filter and not _ovl_match(s['name'], ovl_filter):
                continue
            for n, v in s['syms']:
                if rx.search(n) and (s['name'], n) not in seen:
                    seen.add((s['name'], n))
                    print(f'data {s["name"]:<20} {v:08X} {n}')


if __name__ == '__main__':
    main(sys.argv[1:])
