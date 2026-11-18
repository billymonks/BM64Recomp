"""Build the decomp-layout ROM that N64Recomp expects (bm64.decomp.us.z64) from the retail USA ROM.

Layout (from BM64Syms section tables): the retail 8 MB image unchanged, followed by each overlay bank's
header and its sub-overlays stored decompressed at the ROM offsets listed in BM64Syms/dump.toml.
Bank header contents are not code and are not read by N64Recomp; they are copied from the retail bank area.

Usage: python make_decomp_rom.py [output_path]   (default: <repo>/bm64.decomp.us.z64)
"""
import os, sys
from syms import ROM, FUNC_SECS, DATA_SECS, REPO
import ovl as O


def build():
    if len(ROM) != 0x800000:
        raise SystemExit(f'expected an 8 MB retail ROM, got {len(ROM):#x} bytes')
    if ROM[0x20:0x2B] != b'BOMBERMAN64':
        raise SystemExit('ROM header is not BOMBERMAN64 (big-endian .z64 expected)')
    end = max(s['rom'] + s['size'] for s in FUNC_SECS if s['rom'] is not None)
    out = bytearray(end)
    out[:len(ROM)] = ROM
    for s in DATA_SECS:
        if s['name'].endswith('_header') and s['name'].startswith('.ovl_'):
            bank = int(s['name'].split('_')[1])
            area = 0x100000 + bank * 0x20000
            out[s['rom']:s['rom'] + s['size']] = ROM[area:area + s['size']]
    for name in O.TABLE:
        sec = next(s for s in FUNC_SECS if s['name'] == name)
        data = O.get(name)
        out[sec['rom']:sec['rom'] + len(data)] = data
    return bytes(out)


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'bm64.decomp.us.z64')
    data = build()
    with open(path, 'wb') as f:
        f.write(data)
    print(f'wrote {path} ({len(data):#x} bytes)')
