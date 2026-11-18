"""Survey: OS, scheduler and audio objects by call site. For every call to a libultra/audio/thread API in main and
boot code, prints the constant pointer arguments ($a0-$a3) and the symbol they point at. Use it to find
non-simulation objects (threads, message queues, scheduler, audio heap) inside the state-hash ranges.

Usage: python osobjs.py [lo hi]     only report arguments in [lo, hi) (default: everything in RDRAM)
"""
import sys, struct
from syms import *
from bmdis import IMAGES, func_at

# function -> names of the pointer arguments worth reporting, by register ($a0..$a3)
APIS = {
    'osCreateThread': ('thread', None, 'entry', 'arg'),
    'osStartThread': ('thread',),
    'osCreateMesgQueue': ('queue', 'msgbuf', None),
    'osSendMesg': ('queue',), 'osRecvMesg': ('queue', 'msg_out'), 'osJamMesg': ('queue',),
    'osSetEventMesg': (None, 'queue'), 'osViSetEvent': ('queue',),
    'osCreateScheduler': ('sched', 'stack'), 'osScAddClient': ('sched', 'client', 'queue'), 'osScGetCmdQ': ('sched',),
    'osCreateViManager': (), 'osCreatePiManager': (None, 'queue', 'msgbuf'),
    'osPiStartDma': ('iomesg',), 'osEPiStartDma': (None, 'iomesg'),
    'osSetTimer': ('timer',),
    'osAiSetNextBuffer': ('buffer',),
    'alInit': ('globals', 'config'), 'alHeapInit': ('heap', 'base'), 'alHeapDBAlloc': (None, None, 'heap'),
    'osContStartReadData': ('queue',), 'osContInit': ('queue', 'bitpattern', 'status'),
    'osEepromProbe': ('queue',), 'osEepromLongRead': ('queue',), 'osEepromLongWrite': ('queue',),
    'osPfsInitPak': ('queue', 'pfs'),
    'ThreadProc_Init': ('thread',), 'ThreadProc_Add': (),
    'func_80237AE4': ('client', 'queue'), 'func_80237A70': ('queue',), 'func_80237F44': ('queue',),
}


def track(base, data, fstart, fsize, targets):
    """Linear scan with a tiny constant tracker; returns [(call_addr, target_name, {reg: value})]."""
    saved = (16, 17, 18, 19, 20, 21, 22, 23, 30)
    regs = {}
    out = []
    pending = None
    for off in range(fstart - base, fstart - base + fsize, 4):
        addr = base + off
        w = struct.unpack('>I', data[off:off + 4])[0]
        op, rs, rt, rd, funct = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 63
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 0x0F:  # lui
            regs[rt] = (imm << 16) & 0xFFFFFFFF
        elif op == 0x09 and rs in regs:  # addiu
            regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
        elif op == 0x0D and rs in regs:  # ori
            regs[rt] = regs[rs] | imm
        elif op == 0 and funct in (0x21, 0x25) and (rs in regs) != (rt in regs) and 0 in (rs, rt):  # move
            regs[rd] = regs[rs if rs else rt]
        elif op == 0 and funct not in (0x08, 0x09):
            regs.pop(rd, None)
        elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x21, 0x23, 0x24, 0x25, 0x27):
            regs.pop(rt, None)
        if pending:
            call, name = pending
            out.append((call, name, {r: regs[r] for r in (4, 5, 6, 7) if r in regs}))
            pending = None
            regs = {k: v for k, v in regs.items() if k in saved}
        if op == 0x03:  # jal
            t = ((w & 0x3FFFFFF) << 2) | (addr & 0xF0000000)
            if t in targets:
                pending = (addr, targets[t])
            else:
                pending = ('', None)
    return [o for o in out if o[1]]


def main(lo, hi):
    targets = {}
    for name in APIS:
        for sec, v, sz in func_by_name.get(name, []):
            if not sec.startswith('.ovl'):
                targets[v] = name
    for v, (n, sz, sec) in func_by_addr.items():
        base = n.split('__0x')[0] if n.endswith('__secure_call') else None
        if base in APIS:
            targets[v] = base
    rows = []
    for base, data, s in IMAGES:
        for fname, fv, fsz in s['funcs']:
            for call, name, regs in track(base, data, fv, fsz, targets):
                labels = APIS[name]
                for i, r in enumerate((4, 5, 6, 7)):
                    if i < len(labels) and labels[i] and r in regs and lo <= regs[r] < hi:
                        v = regs[r]
                        rows.append((v, labels[i], name, fname, call))
    for v, label, api, fname, call in sorted(set(rows)):
        print(f'{v:08X} {name_of(v) or "":<28} {label:<8} {api:<20} from {fname} @ {call:08X}')


if __name__ == '__main__':
    a = sys.argv[1:]
    main(int(a[0], 16) if a else 0x80000000, int(a[1], 16) if len(a) > 1 else 0x80800000)
