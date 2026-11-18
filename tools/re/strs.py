"""Decode the byte-shifted menu strings in overlays (or `main`). `python strs.py [.ovl_3_subovl_1 ...|main]`"""
import re, sys
from syms import *
import ovl as O


def dec(s):
    return ''.join(chr(ord(c) + 7) if 0x20 <= ord(c) + 7 < 0x7f else '?' for c in s)


def runs(data, minlen=5):
    for m in re.finditer(rb'[\x19-\x77]{%d,}' % minlen, data):
        yield m.start(), m.group().decode('latin1')


def plausible(t):
    letters = sum(c.isalpha() for c in t)
    return letters >= max(4, int(len(t) * 0.7)) and re.search(r'[A-Z]{3}', t)

targets = sys.argv[1:] or sorted(O.TABLE, key=lambda s: tuple(int(x) for x in re.findall(r'\d+', s)))
for sec in targets:
    if sec == 'main':
        s = sec_for('.main_code'); data = ROM[s['rom']:s['rom'] + s['size']]
    else:
        data = O.get(sec)
    found = []
    for off, t in runs(data):
        d = dec(t)
        if plausible(d) and not plausible(t):
            found.append(d.strip())
    print(sec, '|', ' / '.join(dict.fromkeys(found))[:600])
