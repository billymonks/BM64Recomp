"""Classify the first argument ($a0) passed at every call site of a set of functions (main + all overlays).

Usage: python argclass.py [hexaddr ...]   (defaults to the player accessor set below)
"""
import re, collections, struct, sys
import rabbitizer
from syms import *
import ovl as O
import bmdis
from bmdis import IMAGES, func_at
from stubs import stub_map

DEFAULT_TARGETS = {0x802443F4: 'getPlayerObj', 0x80250338: 'getPlayerPos', 0x80243D2C: 'getHearts', 0x802442A8: 'getP4C',
                   0x80250E2C: 'func_80250E2C', 0x80277D20: 'setP18', 0x80243F2C: 'setControlType', 0x802442D0: 'getFire'}


def classify(img_base, data, call_addr):
    # look at delay slot then backwards up to 12 instrs for the last write of $a0 (reg 4)
    def ins_at(a):
        w = struct.unpack('>I', data[a - img_base:a - img_base + 4])[0]
        return rabbitizer.Instruction(w, vram=a)
    order = [call_addr + 4] + [call_addr - 4 * i for i in range(1, 14)]
    for a in order:
        if a < img_base:
            break
        i = ins_at(a)
        mn = i.getOpcodeName()
        if a != call_addr + 4 and (i.isJump() or i.isBranch()):
            return 'unknown(branch)'
        try:
            dst = i.rt.value if mn in ('addiu', 'ori', 'lw', 'lh', 'lhu', 'lb', 'lbu', 'andi', 'slti', 'lui') else i.rd.value
        except Exception:
            continue
        if dst != 4:
            continue
        t = i.disassemble()
        if mn == 'addiu' and i.rs.value == 0:
            return f'const {i.getProcessedImmediate()}'
        if mn in ('or', 'addu') and (i.rs.value == 0 or i.rt.value == 0):
            src = i.rt.value if i.rs.value == 0 else i.rs.value
            if src == 0:
                return 'const 0'
            return f'reg ${src}'
        if mn == 'lw':
            return f'load {t.split()[-1]}'
        return f'expr {t}'
    return 'unknown'


def main(targets):
    res = collections.defaultdict(collections.Counter)
    for sec in sorted(O.TABLE):
        O.load(sec)
        sm = stub_map(sec)
        base, data, s = bmdis.OVL_IMAGES[sec]
        for off in range(0, len(data), 4):
            w = struct.unpack('>I', data[off:off + 4])[0]
            if (w >> 26) == 3:
                t = ((w & 0x3FFFFFF) << 2) | 0x80000000
                t = sm.get(t, t)
                if t in targets:
                    res[targets[t]][classify(base, data, base + off)] += 1
    for base, data, s in IMAGES:
        for off in range(0, len(data), 4):
            w = struct.unpack('>I', data[off:off + 4])[0]
            if (w >> 26) == 3:
                t = ((w & 0x3FFFFFF) << 2) | 0x80000000
                if t in targets:
                    res[targets[t] + ' (main)'][classify(base, data, base + off)] += 1
    for k, v in res.items():
        print(k, dict(v.most_common(12)))


if __name__ == '__main__':
    main({int(a, 16): a for a in sys.argv[1:]} if len(sys.argv) > 1 else DEFAULT_TARGETS)
