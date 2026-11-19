"""Repoint main-code g_Players references at 0x80F00000 and widen the slot loops.

Each reference is a lui whose result is used once, before that register is written again, to form an
address in the original 5-slot array. The lui immediate becomes 0x80F0 and the paired offset becomes
the byte offset within the array. Loop ends of 4 and 5 slots become 8. g_ComputerPlayerMovement and
the func_80241BB0 side table move with them. Run from tools/re:

    python relocate_players.py          # refresh the marked block in bm64.us.toml
    python relocate_players.py --check  # the marked block covers every reference
"""
import struct
import sys
import rabbitizer
from bmdis import IMAGES, func_at, read_word

OLD = 0x800AEDF0
# Inclusive of the exclusive end pointer (base + 5 * stride). Loops compare against it.
OLD_END = 0x800AF1D8
# These write or form D_800AF1D8 as the next table, not as a player-loop bound.
# func_8024ABA8 indexes that table by words; the other three are func_802506B8.
SKIP_USES = {0x8025072C, 0x802507DC, 0x80250C48, 0x8024ABC4, 0x8024AD2C, 0x8024AEF8}
NEW = 0x80F00000
SLOTS = 8
STRIDE = 0xC8
# addiu instructions that build a 4-slot (base+0x320) or 5-slot (base+0x3E8) loop end.
WIDEN_ENDS = {0x802458A0, 0x80245904, 0x80246348, 0x80246364, 0x80250148}
# Counted loops: (address, old immediate, new immediate). Each steps a player by 0xC8,
# or writes one field of every slot. The 4-port controller map in func_8023D0F8 stays at 4.
WIDEN_IMM = (
    (0x8023C228, 4, SLOTS),
    (0x80241C48, 4, SLOTS),
    (0x80242A1C, 4, SLOTS),
    (0x80242A6C, 4, SLOTS),
    (0x80242C34, 4, SLOTS),
    (0x802441D8, 5, SLOTS),
    (0x80245D40, 5, SLOTS),
    (0x802461C8, 4, SLOTS),
    (0x80246FF4, 4, SLOTS),
    (0x80247200, 4, SLOTS),
    (0x80250020, 5, SLOTS),
    (0x80277BF4, 5, SLOTS),
    (0x80278F94, 5, SLOTS),
)
# g_ComputerPlayerMovement: 5 entries of 0x10 in BSS. Eight entries follow the player array.
MOVE_OLD = 0x802ACF08
MOVE_END = 0x802ACF58
MOVE_NEW = 0x80F00640
# func_80241BB0 writes a 0x14-stride side table while it walks players. Initialized data.
HUD_OLD = 0x802A07D4
HUD_END = 0x802A0838
HUD_NEW = 0x80F006C0
MEM = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld', 'sb', 'sh', 'sw', 'sd',
       'lwc1', 'swc1', 'ldc1', 'sdc1', 'lwl', 'lwr', 'swl', 'swr'}
BEGIN = '# BEGIN bm64 relocated player array'
END = '# END bm64 relocated player array'
TOML = __import__('os').path.normpath(__import__('os').path.join(
    __import__('os').path.dirname(__file__), '..', '..', 'bm64.us.toml'))


def scan():
    uses = []
    tainted = set()
    indexed_ops = {'addu', 'add', 'or'}
    for base, data, sec in IMAGES:
        if sec['name'] != '.main_code':
            continue
        pure = {}
        indexed = {}
        for off in range(0, len(data), 4):
            addr = base + off
            word = struct.unpack('>I', data[off:off + 4])[0]
            ins = rabbitizer.Instruction(word, vram=addr)
            mn = ins.getOpcodeName()
            if mn == 'lui':
                pure[ins.rt.value] = (addr, (ins.getProcessedImmediate() << 16) & 0xFFFFFFFF, word)
                indexed.pop(ins.rt.value, None)
                continue
            if mn in indexed_ops:
                try:
                    rd = ins.rd.value
                    rs = ins.rs.value
                    rt = ins.rt.value
                except Exception:
                    rd = rs = rt = None
                base_reg = rs if rs in pure else rt if rt in pure else None
                if base_reg is not None and rd:
                    indexed[rd] = pure[base_reg]
                    pure.pop(rd, None)
                    continue
            if mn in ('addiu', 'ori') or mn in MEM:
                try:
                    rs = ins.rs.value
                except Exception:
                    rs = None
                imm = ins.getProcessedImmediate() if rs is not None else 0
                if mn == 'ori':
                    imm &= 0xFFFF
                if rs in pure:
                    formed = (pure[rs][1] + imm) & 0xFFFFFFFF
                    if OLD <= formed <= OLD_END:
                        uses.append((pure[rs][0], pure[rs][2], addr, word, formed, mn))
                    elif 0x80000000 <= formed < 0xC0000000:
                        tainted.add(pure[rs][0])
                elif rs in indexed and mn in MEM:
                    formed = (indexed[rs][1] + imm) & 0xFFFFFFFF
                    if OLD <= formed <= OLD_END:
                        uses.append((indexed[rs][0], indexed[rs][2], addr, word, formed, mn))
                    elif 0x80000000 <= formed < 0xC0000000:
                        tainted.add(indexed[rs][0])
            if mn != 'lui' and mn not in indexed_ops:
                for pred, attr in ((ins.modifiesRt, 'rt'), (ins.modifiesRd, 'rd')):
                    try:
                        if pred():
                            reg = getattr(ins, attr).value
                            if reg != 0:
                                pure.pop(reg, None)
                                indexed.pop(reg, None)
                    except Exception:
                        pass
    safe = [u for u in uses if u[0] not in tainted and u[2] not in SKIP_USES]
    bad = [u for u in uses if u[0] in tainted and u[2] not in SKIP_USES]
    return safe, bad


def new_words(lui_word, use_word, formed, use_addr):
    offset = formed - OLD
    if use_addr in WIDEN_ENDS:
        if offset not in (0x320, 0x3E8):
            raise SystemExit(f'{use_addr:#x} is not a 4- or 5-slot end (offset {offset:#x})')
        offset = SLOTS * STRIDE
    if not 0 <= offset < 0x800:
        raise SystemExit(f'offset {offset:#x} from {formed:#x} does not fit the new array')
    return (lui_word & 0xFFFF0000) | 0x80F0, (use_word & 0xFFFF0000) | (offset & 0xFFFF)


def scan_span(old, end, new_base):
    """Same lui/use pairing as scan(), for a half-open address span relocated to new_base."""
    uses = []
    tainted = set()
    indexed_ops = {'addu', 'add', 'or'}
    for base, data, sec in IMAGES:
        if sec['name'] != '.main_code':
            continue
        pure = {}
        indexed = {}
        for off in range(0, len(data), 4):
            addr = base + off
            word = struct.unpack('>I', data[off:off + 4])[0]
            ins = rabbitizer.Instruction(word, vram=addr)
            mn = ins.getOpcodeName()
            if mn == 'lui':
                pure[ins.rt.value] = (addr, (ins.getProcessedImmediate() << 16) & 0xFFFFFFFF, word)
                indexed.pop(ins.rt.value, None)
                continue
            if mn in indexed_ops:
                try:
                    rd = ins.rd.value
                    rs = ins.rs.value
                    rt = ins.rt.value
                except Exception:
                    rd = rs = rt = None
                base_reg = rs if rs in pure else rt if rt in pure else None
                if base_reg is not None and rd:
                    indexed[rd] = pure[base_reg]
                    pure.pop(rd, None)
                    continue
            if mn in ('addiu', 'ori') or mn in MEM:
                try:
                    rs = ins.rs.value
                except Exception:
                    rs = None
                imm = ins.getProcessedImmediate() if rs is not None else 0
                if mn == 'ori':
                    imm &= 0xFFFF
                if rs in pure:
                    formed = (pure[rs][1] + imm) & 0xFFFFFFFF
                    if old <= formed < end:
                        uses.append((pure[rs][0], pure[rs][2], addr, word, formed, mn))
                    elif 0x80000000 <= formed < 0xC0000000:
                        tainted.add(pure[rs][0])
                elif rs in indexed and mn in MEM:
                    formed = (indexed[rs][1] + imm) & 0xFFFFFFFF
                    if old <= formed < end:
                        uses.append((indexed[rs][0], indexed[rs][2], addr, word, formed, mn))
                    elif 0x80000000 <= formed < 0xC0000000:
                        tainted.add(indexed[rs][0])
            if mn != 'lui' and mn not in indexed_ops:
                for pred, attr in ((ins.modifiesRt, 'rt'), (ins.modifiesRd, 'rd')):
                    try:
                        if pred():
                            reg = getattr(ins, attr).value
                            if reg != 0:
                                pure.pop(reg, None)
                                indexed.pop(reg, None)
                    except Exception:
                        pass
    safe = [u for u in uses if u[0] not in tainted]
    bad = [u for u in uses if u[0] in tainted]
    return safe, bad


def relocated_pair(lui_word, use_word, formed, old, new_base):
    offset = (new_base - NEW) + (formed - old)
    if not 0 <= offset < 0x8000:
        raise SystemExit(f'offset {offset:#x} from {formed:#x} does not fit {new_base:#x}')
    return (lui_word & 0xFFFF0000) | 0x80F0, (use_word & 0xFFFF0000) | (offset & 0xFFFF)


def i_imm(word, imm):
    return (word & 0xFFFF0000) | (imm & 0xFFFF)


def emit_ins(lines, addr, value, note):
    fn = func_at(addr)
    if not fn:
        raise SystemExit(f'no function contains {addr:#x}')
    lines.append('')
    lines.append('[[patches.instruction]]')
    lines.append(f'func = "{fn[0]}"')
    lines.append(f'vram = {addr:#x}')
    lines.append(f'value = {value:#x} # {note}')


def stack_frame(lines, start, size, allocs, grow, keep):
    """Grow a frame by `grow` bytes. `allocs` are the addiu sp, ±frame instructions.
    Offsets in `keep` stay put (their slots move down with sp). Every other sp offset
    increases by `grow`, so those slots keep their old absolute addresses.
    """
    for addr in allocs:
        word = read_word(addr)
        ins = rabbitizer.Instruction(word, vram=addr)
        imm = ins.getProcessedImmediate()
        if ins.getOpcodeName() != 'addiu' or ins.rs.value != 29:
            raise SystemExit(f'{addr:#x} is not a stack allocation')
        emit_ins(lines, addr, i_imm(word, imm + (-grow if imm < 0 else grow)),
                 f'stack frame {imm:+#x} -> {imm + (-grow if imm < 0 else grow):+#x}')
    for off in range(0, size, 4):
        addr = start + off
        if addr in allocs:
            continue
        word = read_word(addr)
        ins = rabbitizer.Instruction(word, vram=addr)
        mn = ins.getOpcodeName()
        if mn not in MEM and mn != 'addiu':
            continue
        try:
            if ins.rs.value != 29:
                continue
            imm = ins.getProcessedImmediate()
        except Exception:
            continue
        if imm in keep:
            continue
        emit_ins(lines, addr, i_imm(word, imm + grow), f'{mn} sp{imm:+#x} -> sp{imm + grow:+#x}')


def emit_span(lines, safe, old, new_base, label):
    seen_lui = set()
    for lui_addr, lui_word, use_addr, use_word, formed, mn in safe:
        if old == OLD:
            lui_new, use_new = new_words(lui_word, use_word, formed, use_addr)
            shown = SLOTS * STRIDE if use_addr in WIDEN_ENDS else formed - old
        else:
            lui_new, use_new = relocated_pair(lui_word, use_word, formed, old, new_base)
            shown = formed - old
        for addr, value in ((lui_addr, lui_new), (use_addr, use_new)):
            if addr in seen_lui and addr == lui_addr:
                continue
            if addr == lui_addr:
                seen_lui.add(addr)
            emit_ins(lines, addr, value, f'{mn} {label}+{shown:#x}')


def emit(safe, move, hud):
    lines = [BEGIN, '# Generated by tools/re/relocate_players.py. Do not edit by hand.',
             '# g_Players lives at 0x80F00000. Loops visit 8 slots (stride 0xC8).',
             '# g_ComputerPlayerMovement follows it at 0x80F00640 (8 x 0x10).',
             '# func_80241BB0\'s side table is at 0x80F006C0.']
    emit_span(lines, safe, OLD, NEW, 'g_Players')
    emit_span(lines, move, MOVE_OLD, MOVE_NEW, 'g_ComputerPlayerMovement')
    emit_span(lines, hud, HUD_OLD, HUD_NEW, 'D_802A07D4')
    lines.append('')
    lines.append('# Counted player loops. The immediate is the slot count.')
    for addr, old_imm, new_imm in WIDEN_IMM:
        word = read_word(addr)
        ins = rabbitizer.Instruction(word, vram=addr)
        if ins.getProcessedImmediate() != old_imm:
            raise SystemExit(f'{addr:#x} immediate is {ins.getProcessedImmediate()}, expected {old_imm}')
        emit_ins(lines, addr, i_imm(word, new_imm), f'{ins.getOpcodeName()} slot count {old_imm} -> {new_imm}')
    lines.append('')
    lines.append('# func_80246078 keeps per-slot words at the top of a 0x1D8 frame. Eight slots need')
    lines.append('# four more bytes there, so the frame grows and the saved registers move with it.')
    stack_frame(lines, 0x80246078, 0x58C, (0x80246078, 0x80246600), 0x10,
                {0x138, 0x13C, 0x144, 0x158, 0x16C, 0x180, 0x194, 0x1AC, 0x1B8, 0x1BC})
    lines.append('')
    lines.append('# func_802500D4 walks a word per slot from sp+0x90. The next slot (sp+0xA0) stays put')
    lines.append('# relative to the caller; the frame grows so eight words fit in front of it.')
    stack_frame(lines, 0x802500D4, 0x1E8, (0x802500D4, 0x802502B8), 0x10, {0x90})
    lines.append('')
    lines.append('[[patches.hook]]')
    lines.append('func = "g_StopSound"')
    lines.append('before_vram = 0x8026C0A8')
    lines.append('text = """')
    lines.append('    extern int bm64_sound_speculative(void);')
    lines.append('    if (bm64_sound_speculative()) {')
    lines.append('        return;')
    lines.append('    }')
    lines.append('"""')
    lines.append('')
    lines.append('[[patches.hook]]')
    lines.append('func = "func_80245CDC"')
    lines.append('before_vram = 0x80245CDC')
    lines.append('text = """')
    lines.append('    extern void bm64_coop_before_stage(uint8_t* rdram);')
    lines.append('    bm64_coop_before_stage(rdram);')
    lines.append('"""')
    lines.append(END)
    return '\n'.join(lines) + '\n'


def splice(text, block):
    if BEGIN in text:
        pre, rest = text.split(BEGIN, 1)
        _, post = rest.split(END, 1)
        post = post.lstrip('\n')
        return pre + block + post
    if not text.endswith('\n'):
        text += '\n'
    return text + '\n' + block


def main():
    safe, bad = scan()
    if bad:
        print(f'{len(bad)} tainted references; not writing', file=sys.stderr)
        return 1
    indexed = [u for u in safe if u[2] == 0x80250354]
    if len(indexed) != 1:
        print('getPlayerPos indexed load was not found', file=sys.stderr)
        return 1
    known = [u for u in safe if u[2] == 0x80245D38]
    if len(known) != 1:
        print('stage-setup g_Players addiu was not found', file=sys.stderr)
        return 1
    lui_new, use_new = new_words(known[0][1], known[0][3], known[0][4], known[0][2])
    if lui_new != 0x3C1180F0 or use_new != 0x26310000:
        print(f'unexpected stage-setup encoding {lui_new:#x} {use_new:#x}', file=sys.stderr)
        return 1
    if {u[2] for u in safe} & SKIP_USES:
        print('a next-table reference was not skipped', file=sys.stderr)
        return 1
    ends = {u[2] for u in safe} & WIDEN_ENDS
    if ends != WIDEN_ENDS:
        print(f'missing widened loop ends: {sorted(WIDEN_ENDS - ends)}', file=sys.stderr)
        return 1
    move, move_bad = scan_span(MOVE_OLD, MOVE_END, MOVE_NEW)
    hud, hud_bad = scan_span(HUD_OLD, HUD_END, HUD_NEW)
    if move_bad or hud_bad or len(move) != 6 or len(hud) != 2:
        print(f'companion refs move={len(move)} hud={len(hud)} tainted={len(move_bad)+len(hud_bad)}', file=sys.stderr)
        return 1
    block = emit(safe, move, hud)
    if '--write' not in sys.argv and '--check' not in sys.argv:
        print(f'{len(safe)} references found. Not written.')
        print('The toml already has a generated block. Pass --write to replace it.')
        return 0
    if '--check' in sys.argv:
        current = open(TOML, encoding='utf-8').read()
        if BEGIN not in current:
            print('marked block missing', file=sys.stderr)
            return 1
        body = current.split(BEGIN, 1)[1].split(END, 1)[0]
        want = []
        for group in (safe, move, hud):
            for u in group:
                want.append(u[0])
                want.append(u[2])
        for addr, _, _ in WIDEN_IMM:
            want.append(addr)
        missing = [a for a in want if f'vram = {a:#x}' not in body]
        leaked = [a for a in SKIP_USES if f'vram = {a:#x}' in body]
        if missing or leaked:
            print(f'{len(missing)} references missing, {len(leaked)} next-table refs rewritten', file=sys.stderr)
            return 1
        print(f'check ok, {len(safe)} player refs, {len(move)} movement refs, {len(hud)} side-table refs, {len(WIDEN_IMM)} widened loops')
        return 0
    current = open(TOML, encoding='utf-8').read()
    open(TOML, 'w', encoding='utf-8', newline='\n').write(splice(current, block))
    print(f'wrote {len(safe)} player refs, {len(move)} movement refs, {len(hud)} side-table refs, {len(WIDEN_IMM)} widened loops')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
