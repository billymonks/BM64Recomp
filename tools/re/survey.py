"""Survey: section list and named symbols in main code. `python survey.py`"""
import re
from syms import *

for s in FUNC_SECS:
    if not s['funcs']:
        continue
    named = [f for f in s['funcs'] if not re.match(r'(func|D)_[0-9A-F]{8}', f[0])]
    if not s['name'].startswith('.ovl'):
        print(s['name'], hex(s['vram']), hex(s['size']), 'rom', hex(s['rom'] or 0), 'funcs', len(s['funcs']), 'named', len(named))
print('ovl secs', sum(1 for s in FUNC_SECS if s['name'].startswith('.ovl')), 'ovl funcs', sum(len(s['funcs']) for s in FUNC_SECS if s['name'].startswith('.ovl')))
main = sec_for('.main_code')
named = sorted([f for f in main['funcs'] if not re.match(r'(func|D)_[0-9A-F]{8}', f[0])], key=lambda f: f[1])
for n, v, sz in named:
    print(hex(v), hex(sz), n)
print('---- data secs')
for s in DATA_SECS:
    if not s['name'].startswith('.ovl'):
        named = [x for x in s['syms'] if not re.match(r'(D|func)_[0-9A-F]{8}', x[0]) and not x[0].endswith(('_START', '_END', '_VRAM', '_VRAM_END', '_DATA_START', '_DATA_END'))]
        print(s['name'], hex(s['vram']), hex(s['size']), len(s['syms']), [x[0] + '@' + hex(x[1]) for x in named][:80])
