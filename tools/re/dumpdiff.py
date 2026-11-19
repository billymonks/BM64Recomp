"""Diff two RDRAM dumps (BM64_DUMP_TICKS, N64 byte order, base 0x80000000) into address ranges with symbols.

Usage:
  python dumpdiff.py a/dumps/tick_1702.bin b/dumps/tick_1702.bin            differences inside the default hash ranges
  python dumpdiff.py A B --ranges all                                        everywhere in 0x80000000-0x80800000
  python dumpdiff.py A B --ranges legacy                                     the Phase 0 ranges (ovl, bss, main, heap)
  python dumpdiff.py A B --ranges "bss:80063000-800BE848,heap:802B36D0-80400000" --gap 32 --max 200

Dead process stack ranges listed in tick_<n>.mask next to either dump are ignored (--no-mask to compare them too).
Each line: range, size, differing words, region, nearest symbol, and the first differing words (a -> b). Value notes:
  [gfxbuf] both values point into gGfxWork and differ by one buffer (0x27000): gfx double-buffer parity
  [host]   a value that is not an N64 address or small number, e.g. half of a host pointer in an OSThread
"""
import bisect, collections, os, struct, sys
from syms import DATA_SECS

BASE = 0x80000000
GFX_WORK, GFX_WORK_END, GFX_BUF = 0x8014C8D0, 0x8019A8D0, 0x27000
PROC_POOL, PROC_SIZE, PROC_COUNT = 0x80063370, 0x8A0, 0x50

LEGACY = 'ovl:80043000-80063000,bss:80063000-800BEA60,main:8029F5E0-802B36D0,heap:802B36D0-80400000'
DEFAULT = ('ovl:80043000-80063000,bss:80063000-800BD770+800BD818-800BE848,files:8014B000-8014C8D0,data:8029F5E4-8029F608+8029F6D8-802A5300,'
           'gfx:802A5300-802A5330+802A5368-802A5380+802A53BC-802A5550+802A5554-802A5A60,prc:802AB200-802AC3E0,'
           'game:802AC400-802AD120+802AFC20-802B36D0,heap:802B36D0-80400000')

# Known non-simulation objects (see "State hash ranges" in docs/game-internals.md).
REGIONS = [
    (0x8001BFC0, 0x80028460, 'boot BSS (libultra threads, stacks, queues)'),
    (0x80043000, 0x80063000, 'resident overlay'),
    (0x80098C68, 0x80098CF8, 'texture scroll table (func_8022997C)'),
    (0x800BD770, 0x800BD818, 'sound slot table (follows audio-thread state)'),
    (0x800BE848, 0x800BEA60, 'pre-NMI thread: queue, OSThread, stack'),
    (0x800BEA60, 0x80100000, 'post-BSS gap (unhashed; SyncTest did not need it)'),
    (0x80100000, 0x8014B000, 'framebuffers'),
    (0x8014B000, 0x8014C8D0, 'file directory'),
    (GFX_WORK, GFX_WORK_END, 'gGfxWork display lists'),
    (0x8019C2E0, 0x801E56C0, 'audio heap'),
    (0x8029F5E0, 0x8029F5E4, 'gFrontBufferID (render thread)'),
    (0x8029F608, 0x8029F6D8, 'OSScTask x2 (render thread, scheduler)'),
    (0x802A5330, 0x802A5368, 'render/retrace message queues'),
    (0x802A5380, 0x802A53BC, 'gfx buffer parity + render-thread vars'),
    (0x802A5550, 0x802A5554, 'gfx matrix cursor (buffer parity)'),
    (0x802A5A60, 0x802A8920, 'gThreadProcs (render thread OSThread + stack)'),
    (0x802A8920, 0x802A8938, 'ThreadProc globals'),
    (0x802A8938, 0x802A89B0, 'ThreadProc retrace queues, gThreadFrameCount'),
    (0x802A89B0, 0x802AAC40, 'OSSched + scheduler stack'),
    (0x802AAC40, 0x802AB200, 'ThreadProc manager OSThread + stack'),
    (0x802AB200, 0x802AC3E0, 'HuPrc lists and globals'),
    (0x802AC3E0, 0x802AC400, 'controller message queue'),
    (0x802AD120, 0x802AFC20, 'audio globals, audio thread + stack'),
    (0x802B3620, 0x802B36D0, 'malloc bookkeeping'),
    (0x802B36D0, 0x80400000, 'malloc heap'),
    (0x80400000, 0x80800000, 'above heap'),
]


def parse_ranges(spec):
    if spec == 'all':
        return [('all', [(BASE, 0x80800000)])]
    spec = {'legacy': LEGACY, 'default': DEFAULT}.get(spec, spec)
    out = []
    for item in spec.split(','):
        name, _, segs = item.rpartition(':')
        out.append((name or item, [tuple(int(x, 16) for x in s.split('-')) for s in segs.split('+')]))
    return out


def region(addr):
    if PROC_POOL <= addr < PROC_POOL + PROC_SIZE * PROC_COUNT:
        i, off = divmod(addr - PROC_POOL, PROC_SIZE)
        return f'process[{i}] ' + ('stack' if 0x28 <= off < 0x828 else f'+0x{off:X}')
    best = None
    for lo, hi, name in REGIONS:
        if lo <= addr < hi and (best is None or hi - lo < best[1] - best[0]):
            best = (lo, hi, name)
    return best[2] if best else ''


_syms = sorted({v: n for s in DATA_SECS if not s['name'].startswith('.ovl') for n, v in s['syms']
                if BASE <= v < 0x80800000 and not n.endswith(('_START', '_END', '_VRAM', '_VRAM_END'))}.items())
_sym_addrs = [a for a, _ in _syms]


def symbol(addr):
    i = bisect.bisect_right(_sym_addrs, addr) - 1
    if i < 0 or addr - _sym_addrs[i] > 0x10000:
        return ''
    a, n = _syms[i]
    return n.replace('.NON_MATCHING', '') + (f'+0x{addr - a:X}' if addr != a else '')


def note(a, b):
    if GFX_WORK <= a < GFX_WORK_END and GFX_WORK <= b < GFX_WORK_END and abs(a - b) == GFX_BUF:
        return ' [gfxbuf]'
    def odd(v):
        return not (v < 0x10000 or v > 0xFFFF0000 or BASE <= v < 0x80800000 or 0x3C000000 <= v < 0x48000000
                    or 0xBC000000 <= v < 0xC8000000)
    return ' [host]' if odd(a) and odd(b) else ''


def load_mask(dump_path):
    path = os.path.splitext(dump_path)[0] + '.mask'
    if not os.path.isfile(path):
        return []
    return [tuple(int(x, 16) for x in line.split()) for line in open(path) if line.strip()]


def main(argv):
    opts = {'--ranges': 'default', '--gap': '16', '--max': '120', '--words': '4'}
    files = []
    use_mask = True
    while argv:
        a = argv.pop(0)
        if a in opts:
            opts[a] = argv.pop(0)
        elif a == '--no-mask':
            use_mask = False
        else:
            files.append(a)
    if len(files) != 2:
        raise SystemExit(__doc__)
    da, db = (open(f, 'rb').read() for f in files)
    masked = bytearray(len(da) // 4)
    if use_mask:
        for lo, hi in load_mask(files[0]) + load_mask(files[1]):
            masked[(lo - BASE) // 4:(hi - BASE) // 4] = b'\1' * ((hi - lo) // 4)
    gap, limit, nwords = int(opts['--gap'], 0), int(opts['--max']), int(opts['--words'])
    total_ranges = 0
    by_region = collections.Counter()
    for name, segs in parse_ranges(opts['--ranges']):
        diffs = []
        for lo, hi in segs:
            for off in range(lo - BASE, hi - BASE, 4):
                if da[off:off + 4] != db[off:off + 4] and not masked[off // 4]:
                    diffs.append(BASE + off)
        groups = []
        for addr in diffs:
            if groups and addr - groups[-1][-1] <= gap:
                groups[-1].append(addr)
            else:
                groups.append([addr])
        print(f'== {name}: {len(diffs)} differing words in {len(groups)} ranges')
        for g in groups:
            lo, hi = g[0], g[-1] + 4
            by_region[region(lo)] += len(g)
            total_ranges += 1
            if total_ranges > limit:
                continue
            vals = []
            for addr in g[:nwords]:
                a = struct.unpack('>I', da[addr - BASE:addr - BASE + 4])[0]
                b = struct.unpack('>I', db[addr - BASE:addr - BASE + 4])[0]
                vals.append(f'{addr:08X}:{a:08X}->{b:08X}{note(a, b)}')
            print(f'  {lo:08X}-{hi:08X} {hi - lo:6X} {len(g):5}w  {region(lo):<44} {symbol(lo):<28} {" ".join(vals)}')
    if total_ranges > limit:
        print(f'  ... {total_ranges - limit} more ranges (raise --max)')
    print('== differing words per region')
    for r, n in by_region.most_common():
        print(f'  {n:7}  {r or "(unlabelled)"}')


if __name__ == '__main__':
    main(sys.argv[1:])
