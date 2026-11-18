"""Find references to an address range: jal targets plus lui/addiu/load/store address pairs.

Usage:
  python xref.py 0x802AC894 [0x802AC898]                  main + boot code (end defaults to start+1)
  python xref.py .ovl_4_subovl_10 0x802443F4             also scan the named overlay(s)
  python xref.py --all-ovl 0x802443F4                     also scan all 95 overlays; overlay calls made
                                                          through secure-call stubs count as calls to
                                                          the stub's main-code target
"""
import sys, struct
import rabbitizer
from syms import *
from bmdis import IMAGES, OVL_IMAGES, func_at, sym, ensure_overlay

LOADSTORE = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld', 'sb', 'sh', 'sw', 'sd', 'lwc1', 'swc1', 'ldc1', 'sdc1', 'addiu', 'ori', 'lwl', 'lwr', 'swl', 'swr'}


def scan(images, targets_lo, targets_hi, want_jal, stub_maps=None):
    """Return (addr, section, mnemonic, target) for every reference into [targets_lo, targets_hi).

    stub_maps: optional {section: {stub_addr: main_target}}; jal to a stub is reported as a jal to its target.
    """
    res = []
    for base, data, s in images:
        hi = {}
        stubs = (stub_maps or {}).get(s['name'], {})
        for off in range(0, len(data), 4):
            addr = base + off
            w = struct.unpack('>I', data[off:off + 4])[0]
            ins = rabbitizer.Instruction(w, vram=addr)
            mn = ins.getOpcodeName()
            if ins.isJumpWithAddress():
                t = ins.getInstrIndexAsVram()
                t = stubs.get(t, t)
                if want_jal and isinstance(t, int) and targets_lo <= t < targets_hi:
                    res.append((addr, s['name'], mn, t))
                continue
            if mn == 'lui':
                hi[ins.rt.value] = (ins.getProcessedImmediate() << 16) & 0xFFFFFFFF
                continue
            if mn in LOADSTORE:
                try:
                    b = ins.rs.value
                except Exception:
                    continue
                if b in hi:
                    imm = ins.getProcessedImmediate()
                    if mn == 'ori':
                        imm &= 0xFFFF
                    a = (hi[b] + imm) & 0xFFFFFFFF
                    if targets_lo <= a < targets_hi:
                        res.append((addr, s['name'], mn, a))
    return res


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    ovls = []
    stub_maps = {}
    if args[0] == '--all-ovl':
        args.pop(0)
        import ovl as O
        ovls = sorted(O.TABLE)
    while args and args[0].startswith('.ovl'):
        ovls.append(args.pop(0))
    if ovls:
        from stubs import stub_map
        for o in ovls:
            ensure_overlay(o)
            stub_maps[o] = stub_map(o)
    lo = int(args[0], 16)
    hi_ = int(args[1], 16) if len(args) > 1 else lo + 1
    imgs = list(IMAGES) + [OVL_IMAGES[o] for o in ovls]
    for addr, sec, mn, t in scan(imgs, lo, hi_, True, stub_maps):
        f = func_at(addr, sec if sec.startswith('.ovl') else None)
        print(f'{addr:08X} {sec:<18} {(f[0] if f else "?"):<28} {mn:<6} {t:08X} {sym(t) or ""}')
