"""Survey: main functions touching fixed g_Players fields, with overlay and main call counts. `python p0api.py`"""
import re, collections
from syms import *
import ovl as O
import bmdis
from bmdis import IMAGES, func_at
from xref import scan
from stubs import stub_map

GP, SZ = 0x800AEDF0, 0xC8
FIELDS = {0x0: 'spawnPos', 0x4: 'spawnPos.y', 0x8: 'spawnPos.z', 0xC: 'dirAngle', 0x10: 'flag', 0x14: 'hearts', 0x18: 'u18', 0x1A: 'bombType',
          0x1C: 'bombCount', 0x20: 'firePower', 0x24: 'controlType', 0x28: 'obj', 0x2C: 'heldObj', 0x30: 'otherObj', 0x40: 'u40', 0x44: 'anim',
          0x48: 'collision', 0x4C: 'u4C', 0x68: 'u68', 0x78: 'virusTimer?', 0x80: 'u80', 0x94: 'u94', 0x98: 'u98', 0xA4: 'uA4', 0xC0: 'uC0'}

# main functions with fixed P0 field refs
refs = scan(IMAGES, GP, GP + SZ, False)
p0funcs = collections.defaultdict(set)
for addr, sec, mn, t in refs:
    f = func_at(addr)
    if f and t != GP:
        p0funcs[f[1]].add(t - GP)
# also count main-code callers and overlay callers
main_callers = collections.Counter()
for addr, sec, mn, t in scan(IMAGES, 0x80225800, 0x802A5000, True):
    if mn == 'jal' and t in p0funcs:
        main_callers[t] += 1
ovl_callers = collections.Counter()
ovl_where = collections.defaultdict(set)
for sec in O.TABLE:
    O.load(sec)
    sm = stub_map(sec)
    for addr, s, mn, t in scan([bmdis.OVL_IMAGES[sec]], 0x80000000, 0x80400000, True):
        if mn == 'jal' and t in sm and sm[t] in p0funcs:
            ovl_callers[sm[t]] += 1
            ovl_where[sm[t]].add(sec.replace('.ovl_', '').replace('_subovl_', '.'))
rows = sorted(p0funcs, key=lambda a: -(ovl_callers[a] + main_callers[a]))
for a in rows:
    f = func_at(a)
    flds = ','.join(FIELDS.get(o, hex(o)) for o in sorted(p0funcs[a]))
    print(f'{f[0]:<16} size {f[2]:>4X} ovlCalls {ovl_callers[a]:>3} mainCalls {main_callers[a]:>3} fields [{flds}] ovls={len(ovl_where[a])}')
