"""Loads BM64Syms (dump.toml, data_dump.toml) and the retail ROM. Env: BM64_SYMS_ROOT (checkout containing BM64Syms/), BM64_ROM (retail USA .z64)."""
import re, struct, os

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def _find_syms_root():
    env = os.environ.get('BM64_SYMS_ROOT')
    cands = [env] if env else []
    cands += [REPO, os.path.dirname(REPO)]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, 'BM64Syms', 'dump.toml')):
            return c
    raise SystemExit('BM64Syms/dump.toml not found. Run `git submodule update --init BM64Syms` '
                     'or set BM64_SYMS_ROOT to a checkout that has it.')


BASE = _find_syms_root()
ROM_PATH = os.environ.get('BM64_ROM', r"C:\Users\willi\Downloads\Bomberman 64 (USA)\Bomberman 64 (USA).z64")

_sec_re = re.compile(r'\[\[section\]\]\s*name = "([^"]*)"\s*(?:rom = (0x[0-9A-Fa-f]+)\s*)?vram = (0x[0-9A-Fa-f]+)\s*size = (0x[0-9A-Fa-f]+)')
_fn_re = re.compile(r'\{ name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+) \}')
_sym_re = re.compile(r'\{ name = "([^"]+)", vram = (0x[0-9A-Fa-f]+)')


def load_sections(path):
    text = open(path).read()
    parts = text.split('[[section]]')[1:]
    secs = []
    for p in parts:
        m = re.search(r'name = "([^"]*)"', p)
        rom = re.search(r'\nrom = (0x[0-9A-Fa-f]+)', p)
        vram = re.search(r'\nvram = (0x[0-9A-Fa-f]+)', p)
        size = re.search(r'\nsize = (0x[0-9A-Fa-f]+)', p)
        fns = [(n, int(v, 16), int(s, 16)) for n, v, s in _fn_re.findall(p)]
        syms = [(n, int(v, 16)) for n, v in _sym_re.findall(p)] if not fns else []
        secs.append(dict(name=m.group(1), rom=int(rom.group(1), 16) if rom else None,
                         vram=int(vram.group(1), 16), size=int(size.group(1), 16),
                         funcs=fns, syms=syms))
    return secs


FUNC_SECS = load_sections(os.path.join(BASE, 'BM64Syms', 'dump.toml'))
DATA_SECS = load_sections(os.path.join(BASE, 'BM64Syms', 'data_dump.toml'))

# global (non-overlay) name maps
func_by_addr = {}
func_by_name = {}
for s in FUNC_SECS:
    ovl = s['name'].startswith('.ovl')
    for n, v, sz in s['funcs']:
        func_by_name.setdefault(n, []).append((s['name'], v, sz))
        if not ovl:
            func_by_addr[v] = (n, sz, s['name'])

data_by_addr = {}
for s in DATA_SECS:
    if s['name'].startswith('.ovl'):
        continue
    for n, v in s['syms']:
        data_by_addr.setdefault(v, n)

ROM = open(ROM_PATH, 'rb').read()


def sec_for(name):
    for s in FUNC_SECS:
        if s['name'] == name:
            return s


def name_of(addr, ovl=None):
    if ovl:
        for n, v, sz in ovl['funcs']:
            if v == addr:
                return n
    if addr in func_by_addr:
        return func_by_addr[addr][0]
    if addr in data_by_addr:
        return data_by_addr[addr]
    return None
