"""Disassemble functions with symbol, data-reference and secure-call-stub annotations.

Usage:
  python bmdis.py func_8024E088 0x80243DDC            main/boot functions by name or any address inside them
  python bmdis.py func_ovl4_10_80043010               overlay functions by name (overlay loads automatically)
  python bmdis.py .ovl_4_subovl_10 0x80043010         overlay function by address (name the overlay first)
"""
import sys, struct, re
import rabbitizer
from syms import *


# code images: (vram_start, bytes)
IMAGES = []
for s in FUNC_SECS:
    if s['name'] in ('.boot_code', '.main_code') and s['rom'] is not None:
        IMAGES.append((s['vram'], ROM[s['rom']:s['rom'] + s['size']], s))
OVL_IMAGES = {}


def add_overlay_image(secname, data):
    s = sec_for(secname)
    OVL_IMAGES[secname] = (s['vram'], data, s)


def read_word(addr, ovl=None):
    imgs = list(IMAGES)
    if ovl and ovl in OVL_IMAGES:
        imgs.insert(0, OVL_IMAGES[ovl])
    for base, data, s in imgs:
        if base <= addr < base + len(data):
            return struct.unpack('>I', data[addr - base:addr - base + 4])[0]
    return None


def func_at(addr, ovl=None):
    cands = []
    if ovl:
        cands += sec_for(ovl)['funcs']
    for s in FUNC_SECS:
        if s['name'] in ('.boot_code', '.main_code'):
            cands += s['funcs']
    for n, v, sz in cands:
        if v <= addr < v + sz:
            return n, v, sz
    return None


def ensure_overlay(secname):
    if secname not in OVL_IMAGES:
        import ovl as O
        O.load(secname)
        # When run as a script, ovl registers into the imported `bmdis` module, not __main__.
        OVL_IMAGES.update(O.bmdis.OVL_IMAGES)


_stub_cache = {}


def sym(addr, ovl=None):
    o = sec_for(ovl) if ovl else None
    n = name_of(addr, o)
    if n and ovl and n.endswith('__secure_call'):
        if ovl not in _stub_cache:
            from stubs import stub_map, pretty
            _stub_cache[ovl] = {a: pretty(t) for a, t in stub_map(ovl).items()}
        target = _stub_cache[ovl].get(addr)
        if target:
            return f'{n} -> {target}'
    return n


LS = {'lb','lbu','lh','lhu','lw','lwu','ld','sb','sh','sw','sd','lwc1','swc1','ldc1','sdc1','addiu','ori','lwl','lwr','swl','swr'}
def disasm(start, size, ovl=None, out=sys.stdout):
    hi = {}; hi_full=set()
    for off in range(0, size, 4):
        addr = start + off
        w = read_word(addr, ovl)
        ins = rabbitizer.Instruction(w, vram=addr)
        txt = ins.disassemble()
        note = ''
        mn = ins.getOpcodeName()
        if ins.isJumpWithAddress():
            t = ins.getInstrIndexAsVram()
            note = sym(t, ovl) or hex(t)
        elif mn == 'lui':
            hi[ins.rt.value] = (ins.getProcessedImmediate() << 16) & 0xFFFFFFFF
        elif mn in LS:
            try:
                base = ins.rs.value
            except Exception:
                base = None
            if base in hi:
                imm = ins.getProcessedImmediate()
                a = (hi[base] + (imm if mn != 'ori' else imm & 0xFFFF)) & 0xFFFFFFFF
                n = sym(a, ovl)
                note = (n + ' ' if n else '') + hex(a)
                if mn in ('addiu','ori'):
                    hi[ins.rt.value] = a
                    hi_full.add(ins.rt.value)
            if mn not in ('sb','sh','sw','sd','swc1','sdc1','lwc1','ldc1','swl','swr') and mn != 'addiu':
                try:
                    hi.pop(ins.rt.value, None)
                except Exception: pass
        out.write(f'{addr:08X}: {w:08X}  {txt:<40} {note}\n')


def disfunc(name_or_addr, ovl=None):
    if isinstance(name_or_addr, str) and not name_or_addr.startswith('0x'):
        lst = func_by_name[name_or_addr]
        if ovl:
            lst = [l for l in lst if l[0] == ovl] or lst
        secname, v, sz = lst[0]
        if secname.startswith('.ovl'):
            ovl = secname
            ensure_overlay(ovl)
    else:
        a = int(name_or_addr, 16) if isinstance(name_or_addr, str) else name_or_addr
        f = func_at(a, ovl)
        n, v, sz = f
    print(f'==== {sym(v, ovl) or hex(v)} @ {v:08X} size {sz:X}')
    disasm(v, sz, ovl)


if __name__ == '__main__':
    ovl = None
    args = sys.argv[1:]
    if args and args[0].startswith('.ovl'):
        ovl = args.pop(0)
        ensure_overlay(ovl)
    if not args:
        print(__doc__)
    for a in args:
        disfunc(a, ovl)


