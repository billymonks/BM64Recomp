"""Survey: coarse RDRAM map, clustering referenced absolute addresses into 64 KB buckets. `python rammap.py`"""
import collections
from syms import *
from bmdis import IMAGES, func_at
from xref import scan

# cluster every absolute address referenced by main+boot code into 64KB buckets, with referencing function counts
refs = scan(IMAGES, 0x80000000, 0x80800000, False)
b = collections.defaultdict(set)
for addr, sec, mn, t in refs:
    f = func_at(addr)
    b[t >> 16].add(f[0] if f else '?')
for k in sorted(b):
    names = sorted(b[k])
    print(f'{k << 16:08X}-{(k << 16) + 0xFFFF:08X} funcs {len(names):>4}  e.g. {", ".join(names[:6])}')
print()
for n in ('gGfxWork', 'D_80100000', 'D_8019A8D0', 'D_801E56C0', 'D_80063000', 'D_800BEA60', 'D_802B36D0', 'D_802A5300', 'g_Players'):
    hits = [hex(v) for s in DATA_SECS for nn, v in s['syms'] if nn == n]
    print(n, hits)
