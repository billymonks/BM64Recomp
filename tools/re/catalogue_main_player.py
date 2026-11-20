"""Classify main-player call sites for phase 6.2.

Categories come from where the call sits, not from a playthrough:
  camera                 the camera module, 0x80234578-0x80237890
  HUD                    the known HUD functions
  trigger or goal        the hero-sequence functions named in game-internals.md
  rival-bomber control   constant slot 1-3 inside one of the six rival-map overlays
  enemy or boss targeting  story or boss overlay code that follows the main player
  cutscene or script     a main-code function that both reads the hero and calls setMainPlayer
  other                  everything else, including battle slots and menus

    python catalogue_main_player.py            print the markdown catalogue
    python catalogue_main_player.py --emit     also write src/game/bm64_player_cats.inc (by process
                                               entry) and src/game/bm64_player_sites.inc (by call site)
"""
import collections
import os
import re
import struct
import sys

import rabbitizer

import ovl
from bmdis import IMAGES, func_at
from stubs import stub_map
from syms import REPO, sec_for

GET = 0x80243DDC
OBJ = 0x802443F4
POS = 0x80250338
WORD = 0x802AC894
SET_MAIN = 0x8024585C
CAM_LO, CAM_HI = 0x80234578, 0x80237890
HUD = {0x80242270, 0x80241590, 0x80241BB0}
TRIGGER = {0x80250E2C, 0x802506B8, 0x80250DC8, 0x802510C8, 0x802512C0, 0x802512F8}
RIVAL = {('4', '10'), ('5', '7'), ('6', '15'), ('8', '13'), ('9', '5'), ('9', '6')}
CATS = (
    'other',
    'camera',
    'enemy or boss targeting',
    'trigger or goal',
    'cutscene or script',
    'HUD',
    'rival-bomber control',
)
INC = os.path.join(REPO, 'src', 'game', 'bm64_player_cats.inc')
SITES_INC = os.path.join(REPO, 'src', 'game', 'bm64_player_sites.inc')
# Every trigger function moves, animates or sequences the bomber it reads (game-internals.md,
# "Main-player sequences"), so at run time each must act on the hero it was called for. Their
# sites are emitted as "other" explicitly, so the process-entry fallback can't turn them into
# a nearest-bomber lookup.
RESOLVE_AS_OTHER = TRIGGER


def bank_sub(secname):
    # .ovl_4_subovl_10
    parts = secname.split('_')
    if len(parts) < 4:
        return None
    return parts[1], parts[3]


def category(fn_addr, secname, const, calls_set_main):
    if CAM_LO <= fn_addr < CAM_HI:
        return 'camera'
    if fn_addr in HUD:
        return 'HUD'
    if fn_addr in TRIGGER:
        return 'trigger or goal'
    if calls_set_main and fn_addr != SET_MAIN:
        return 'cutscene or script'
    ident = bank_sub(secname) if secname else None
    if const in (1, 2, 3) and ident in RIVAL:
        return 'rival-bomber control'
    if secname and ident and ident[0] in {str(n) for n in range(4, 13)} and const is None:
        return 'enemy or boss targeting'
    return 'other'


def const_a0(data, base, call):
    for back in range(0, 14):
        addr = call - 4 * back
        if addr < base:
            break
        word = struct.unpack('>I', data[addr - base:addr - base + 4])[0]
        ins = rabbitizer.Instruction(word, vram=addr)
        mn = ins.getOpcodeName()
        if back and (ins.isJump() or ins.isBranch()):
            return None
        try:
            dst = ins.rt.value if mn in ('addiu', 'ori', 'lw', 'lh', 'lhu', 'lb', 'lbu') else ins.rd.value
        except Exception:
            continue
        if dst != 4:
            continue
        if mn == 'addiu' and ins.rs.value == 0:
            return ins.getProcessedImmediate()
        return None
    return None


def walk(base, data, secname, stubs, calls_set_main_fns):
    rows = []
    for off in range(0, len(data) - 3, 4):
        addr = base + off
        word = struct.unpack('>I', data[off:off + 4])[0]
        ins = rabbitizer.Instruction(word, vram=addr)
        mn = ins.getOpcodeName()
        kind = None
        const = None
        if ins.isJumpWithAddress():
            target = ins.getInstrIndexAsVram()
            target = stubs.get(target, target)
            if target == GET:
                kind = 'getMainPlayer'
            elif target in (OBJ, POS):
                const = const_a0(data, base, addr)
                if const is None and target == OBJ:
                    kind = 'getPlayerObj(computed)'
                elif const is not None:
                    kind = 'getPlayerObj' if target == OBJ else 'getPlayerPos'
                    kind = f'{kind}({const})'
                else:
                    continue
            elif target == SET_MAIN:
                fn = func_at(addr, secname)
                if fn:
                    calls_set_main_fns.add(fn[1])
        if mn == 'lw':
            try:
                rs = ins.rs.value
            except Exception:
                rs = None
            # A direct read is matched by the caller, which tracks the lui. Handled below.
            rs = rs
        if kind:
            fn = func_at(addr, secname)
            rows.append((secname or '.main_code', fn[0] if fn else '?', fn[1] if fn else 0, addr, kind, const))
    return rows


def direct_reads():
    rows = []
    for base, data, sec in IMAGES:
        if sec['name'] != '.main_code':
            continue
        pure = {}
        for off in range(0, len(data), 4):
            addr = base + off
            word = struct.unpack('>I', data[off:off + 4])[0]
            ins = rabbitizer.Instruction(word, vram=addr)
            mn = ins.getOpcodeName()
            if mn == 'lui':
                pure[ins.rt.value] = (ins.getProcessedImmediate() << 16) & 0xFFFFFFFF
                continue
            if mn == 'lw':
                try:
                    rs = ins.rs.value
                except Exception:
                    rs = None
                if rs in pure:
                    imm = ins.getProcessedImmediate()
                    if ((pure[rs] + imm) & 0xFFFFFFFF) == WORD:
                        fn = func_at(addr)
                        rows.append(('.main_code', fn[0] if fn else '?', fn[1] if fn else 0, addr, 'D_802AC894', None))
            if mn != 'lui':
                for pred, attr in ((ins.modifiesRt, 'rt'), (ins.modifiesRd, 'rd')):
                    try:
                        if pred():
                            reg = getattr(ins, attr).value
                            if reg:
                                pure.pop(reg, None)
                    except Exception:
                        pass
    return rows


def main():
    calls_set = set()
    rows = []
    for base, data, sec in IMAGES:
        if sec['name'] == '.main_code':
            rows += walk(base, data, None, {}, calls_set)
    rows += direct_reads()
    for name in sorted(ovl.TABLE):
        data = ovl.get(name)
        sec = sec_for(name)
        rows += walk(sec['vram'], data, name, stub_map(name), calls_set)
    classified = []
    for secname, fn, fn_addr, addr, kind, const in rows:
        cat = category(fn_addr, secname, const, fn_addr in calls_set)
        classified.append((cat, secname, fn, fn_addr, addr, kind))
    if any(c[0] not in CATS for c in classified):
        raise SystemExit('a call site has no category')
    by = collections.Counter((c[0], c[1]) for c in classified)
    print(f'{len(classified)} call sites')
    print()
    print('| Category | Where | Sites |')
    print('|---|---|---|')
    for (cat, where), n in sorted(by.items()):
        print(f'| {cat} | `{where}` | {n} |')
    print()
    print('Main-code functions:')
    seen = set()
    for cat, secname, fn, fn_addr, addr, kind in classified:
        if secname != '.main_code' or fn in seen:
            continue
        seen.add(fn)
        kinds = sorted({k for _c, _s, f, _fa, _a, k in classified if f == fn})
        print(f'- `{fn}` ({fn_addr:#x}): {cat}. {", ".join(kinds)}')
    if '--emit' in sys.argv:
        enum = {}
        src = open(os.path.join(REPO, 'patches', 'required_patches.c'), encoding='utf-8').read()
        for bank, sub, num in re.findall(r'LOADED_OVL_(\d+)_SUBOVL_(\d+) = (\d+)', src):
            enum[(bank, sub)] = int(num)
        def overlay_key(secname, address):
            if secname == '.main_code' or address >= 0x80200000:
                return 0
            return enum.get(bank_sub(secname))

        funcs = {}
        sites = {}
        for cat, secname, fn, fn_addr, addr, kind in classified:
            if not fn_addr:
                continue
            forced = fn_addr in RESOLVE_AS_OTHER
            run_cat = 'other' if forced else cat
            if kind in ('getMainPlayer', 'D_802AC894') and (run_cat != 'other' or forced):
                # getMainPlayer sees its return address; a direct read is hooked at the lw itself.
                site = addr + 8 if kind == 'getMainPlayer' else addr
                key = overlay_key(secname, site)
                if key is not None:
                    sites[(key, site)] = (CATS.index(run_cat), fn)
            if run_cat == 'other':
                continue
            key = overlay_key(secname, fn_addr)
            if key is None:
                continue
            funcs[(key, fn_addr)] = (CATS.index(run_cat), fn)
        lines = ['// Generated by tools/re/catalogue_main_player.py --emit',
                 '// overlay id (0 = main code), function address, category.',
                 '// Lookup is the running process entry. Overlay addresses repeat, so the id is part of the key.']
        for overlay, addr in sorted(funcs):
            idx, name = funcs[(overlay, addr)]
            lines.append(f'    {{ {overlay}, 0x{addr:08X}u, {idx} }}, // {name}: {CATS[idx]}')
        open(INC, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
        print(f'wrote {len(funcs)} functions to {INC}', file=sys.stderr)
        lines = ['// Generated by tools/re/catalogue_main_player.py --emit',
                 '// overlay id (0 = main code), call site, category. The site is the return address of a',
                 '// getMainPlayer call, or the address of a direct lw of D_802AC894. Checked before the',
                 '// process entry table. Sites not listed fall back to it.']
        for overlay, site in sorted(sites):
            idx, name = sites[(overlay, site)]
            lines.append(f'    {{ {overlay}, 0x{site:08X}u, {idx} }}, // {name}: {CATS[idx]}')
        open(SITES_INC, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
        print(f'wrote {len(sites)} call sites to {SITES_INC}', file=sys.stderr)


if __name__ == '__main__':
    main()
