"""Survey: every main-code function referencing g_Players, grouped by field offset. `python players.py`"""
import collections, sys
from syms import *
from bmdis import IMAGES, func_at
from xref import scan

GP = 0x800AEDF0
SZ = 0xC8
refs = scan(IMAGES, GP - 0x10, GP + 8 * SZ, False)
byfunc = collections.defaultdict(list)
for addr, sec, mn, t in refs:
    f = func_at(addr)
    byfunc[f[0] if f else '?'].append((addr, mn, t - GP))

fixed0 = 0
rows = []
for fn, lst in byfunc.items():
    offs = sorted(set(o for _, _, o in lst))
    p0 = [o for o in offs if 0 <= o < SZ and o not in (0,)]
    base_only = all(o == 0 for o in offs)
    rows.append((fn, len(lst), base_only, offs))
rows.sort(key=lambda r: (r[2], r[0]))
print('functions referencing g_Players:', len(rows))
print('-- base-address-only (likely indexed loops / pointer arithmetic):')
for fn, n, b, offs in rows:
    if b:
        print('  ', fn, n)
print('-- fixed field refs (offset from g_Players[0], i.e. hardcoded player 0 or player N fields):')
for fn, n, b, offs in rows:
    if not b:
        print('  ', fn, n, ' '.join(f'{"P%d+" % (o // SZ) if o >= SZ else ""}{o % SZ:X}' for o in offs))
