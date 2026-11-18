"""Resolve overlay secure-call stubs (`<target>__0xNNNN__secure_call`) to main-code targets: `stub_map(section)`."""
import re
from syms import *


def target_of_stubname(n):
    m = re.match(r'(.+?)__0x[0-9A-Fa-f]+__secure_call$', n)
    if not m:
        return None
    base = m.group(1)
    m2 = re.match(r'func_([0-9A-Fa-f]{8})$', base)
    if m2:
        return int(m2.group(1), 16)
    lst = [x for x in func_by_name.get(base, []) if not x[0].startswith('.ovl')]
    if lst:
        return lst[0][1]
    return base  # name only (e.g. os funcs in boot)


def stub_map(secname):
    out = {}
    for n, v, sz in sec_for(secname)['funcs']:
        t = target_of_stubname(n)
        if t is not None:
            out[v] = t
    return out


def pretty(t):
    if isinstance(t, str):
        return t
    n = name_of(t)
    return n or hex(t)
