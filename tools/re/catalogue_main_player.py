"""Classify getMainPlayer call sites and direct D_802AC894 reads. Prints a markdown table."""
import struct
import rabbitizer
from bmdis import IMAGES, func_at

GET = 0x80243DDC
WORD = 0x802AC894
CAM_LO, CAM_HI = 0x80234578, 0x80237890
HUD = {0x80242270, 0x80241590, 0x80241BB0}
TRIGGER = {0x80250E2C, 0x802506B8, 0x80250DC8, 0x802510C8}
RIVAL = {0x8024585C}


def cat(fn_addr, name):
    if CAM_LO <= fn_addr < CAM_HI:
        return 'camera'
    if fn_addr in HUD:
        return 'HUD'
    if fn_addr in TRIGGER:
        return 'trigger'
    if fn_addr in RIVAL:
        return 'rival-bomber'
    return 'other'


def main_sites():
    rows = []
    for base, data, sec in IMAGES:
        if sec['name'] != '.main_code':
            continue
        for off in range(0, len(data), 4):
            addr = base + off
            word = struct.unpack('>I', data[off:off + 4])[0]
            ins = rabbitizer.Instruction(word, vram=addr)
            mn = ins.getOpcodeName()
            kind = None
            if ins.isJumpWithAddress() and ins.getInstrIndexAsVram() == GET:
                kind = 'getMainPlayer'
            if kind:
                fn = func_at(addr)
                rows.append((kind, fn[0] if fn else '?', fn[1] if fn else 0, addr, cat(fn[1] if fn else 0, fn[0] if fn else '')))
    return rows


if __name__ == '__main__':
    rows = main_sites()
    from collections import Counter
    print('sites', len(rows), Counter(r[4] for r in rows))
    for kind, name, fn, addr, c in rows:
        print(f'| {c} | {kind} | `{name}` | `{addr:08X}` |')
