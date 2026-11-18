"""Survey: per overlay, stage-setup map IDs and constant player-slot uses (setMainPlayer, getPlayerObj, setControlType). `python slots.py`"""
import re, struct, collections, sys

from syms import *
import ovl as O, bmdis
from stubs import stub_map
from argclass import classify
T = {0x8024585C:'setMainPlayer', 0x80243F2C:'setControlType', 0x802443F4:'getPlayerObj', 0x80250E2C:'f50E2C', 0x80245CDC:'stageSetup', 0x802459BC:'spawnPlayer'}
out = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
for sec in sorted(O.TABLE, key=lambda s: tuple(int(x) for x in re.findall(r'\d+', s))):
    O.load(sec); sm = stub_map(sec); base, data, s = bmdis.OVL_IMAGES[sec]
    for off in range(0, len(data), 4):
        w = struct.unpack('>I', data[off:off+4])[0]
        if (w >> 26) == 3:
            t = ((w & 0x3FFFFFF) << 2) | 0x80000000
            t = sm.get(t, t)
            if t in T:
                c = classify(base, data, base+off)
                if 'const' in c and not c.endswith(' 0') or T[t] in ('setControlType','stageSetup','spawnPlayer'):
                    out[sec][T[t]][c] += 1
for sec, d in out.items():
    print(sec.replace('.ovl_','').replace('_subovl_','.'), {k: dict(v) for k, v in d.items()})
