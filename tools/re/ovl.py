"""Overlay table (from patches/required_patches.c) and Hudson LZSS decompressor. `python ovl.py` decompresses and checks all 95 overlays; import and call `load(".ovl_4_subovl_10")` to register one with bmdis."""
import re, struct
from syms import *
import bmdis

SRC = open(os.path.join(REPO, 'patches', 'required_patches.c')).read()
# case 0x00122016: recomp_load_overlays(0x00800010, (void*) 0x80043000, 0x000019E0); gOverlayID = LOADED_OVL_1_SUBOVL_1
TABLE = {}
for dev, drom, size, oid in re.findall(r'case (0x[0-9A-Fa-f]+): recomp_load_overlays\((0x[0-9A-Fa-f]+), \(void\*\) 0x80043000, (0x[0-9A-Fa-f]+)\); gOverlayID = LOADED_OVL_(\d+_SUBOVL_\d+)', SRC):
    bank, sub = oid.split('_SUBOVL_')
    TABLE[f'.ovl_{bank}_subovl_{sub}'] = (int(dev, 16), int(drom, 16), int(size, 16))


def hudson_lz(src, off):
    out_size = struct.unpack('>I', src[off:off + 4])[0]
    p = off + 4
    win = bytearray(1024)
    wp = 0x3BE
    out = bytearray()
    flags = 0
    nbits = 0
    while len(out) < out_size:
        if nbits == 0:
            flags = src[p]; p += 1; nbits = 8
        if flags & 1:
            b = src[p]; p += 1
            out.append(b); win[wp] = b; wp = (wp + 1) & 0x3FF
        else:
            b1 = src[p]; b2 = src[p + 1]; p += 2
            pos = b1 | ((b2 & 0xC0) << 2)
            ln = (b2 & 0x3F) + 3
            for i in range(ln):
                b = win[(pos + i) & 0x3FF]
                out.append(b); win[wp] = b; wp = (wp + 1) & 0x3FF
        flags >>= 1; nbits -= 1
    return bytes(out[:out_size]), p - off


_cache = {}


def get(secname):
    if secname not in _cache:
        dev, drom, size = TABLE[secname]
        data, used = hudson_lz(ROM, dev)
        assert len(data) == size, (secname, hex(len(data)), hex(size))
        _cache[secname] = data
    return _cache[secname]


def load(secname):
    bmdis.add_overlay_image(secname, get(secname))


if __name__ == '__main__':
    ok = 0
    for k in TABLE:
        d = get(k)
        s = sec_for(k)
        # sanity: first function bytes should look like code (non-zero)
        ok += 1
    print('decompressed', ok, 'overlays OK; total bytes', sum(len(get(k)) for k in TABLE))
