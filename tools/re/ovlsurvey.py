"""Survey: per overlay, main-code calls (via stubs), input API use, g_Players refs and strings. `python ovlsurvey.py`"""
import re, struct, collections
import rabbitizer
from syms import *
import ovl as O
import bmdis
from xref import scan
from stubs import stub_map, pretty

INPUT_API = {0x802395A8: 'in_stickX?', 0x80239714: 'in_7714', 0x802399AC: 'in_pressed', 0x802399EC: 'in_99EC', 0x80239CC8: 'in_9CC8',
             0x80239D6C: 'in_9D6C', 0x80239E28: 'in_9E28', 0x80239E88: 'in_held', 0x80239EE8: 'in_stickY', 0x80239F70: 'in_stickX',
             0x80239FF8: 'in_9FF8', 0x8023A038: 'in_status'}
G_PLAYERS = (0x800AEDF0, 0x800AEDF0 + 8 * 0xCC)

def strings(data, minlen=6):
    out = []
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data):
        out.append(m.group().decode())
    return out

for sec in sorted(O.TABLE, key=lambda s: tuple(int(x) for x in re.findall(r'\d+', s))):
    O.load(sec)
    img = bmdis.OVL_IMAGES[sec]
    refs = scan([img], 0x80000000, 0x80400000, True)
    sm = stub_map(sec)
    calls = collections.Counter()
    data_refs = collections.Counter()
    inp = collections.Counter()
    nplayers = 0
    for addr, s, mn, t in refs:
        if mn == 'jal':
            if t in sm: t = sm[t]
            if isinstance(t, str):
                calls[t] += 1; continue
            if t in INPUT_API:
                inp[INPUT_API[t]] += 1
            if not (0x80043000 <= t < 0x80060000):
                calls[pretty(t)] += 1
        else:
            if G_PLAYERS[0] <= t < G_PLAYERS[1]:
                nplayers += 1
    st = [x for x in strings(img[1]) if not re.fullmatch(r'[\W\d_]+', x)][:6]
    nf = len(sec_for(sec)['funcs'])
    top = ', '.join(f'{k}x{v}' for k, v in calls.most_common(8))
    print(f'{sec:<20} size {len(img[1]):>6X} funcs {nf:>4} gPlayersRefs {nplayers:>3} input {dict(inp)} | {top} | {st}')

