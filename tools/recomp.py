"""Static recompiler: GALAGA.EXE (16-bit real mode, 286) -> C.

Every decoded instruction becomes a `case`/label inside one C function per code
segment.  Direct jumps are gotos, near calls push the return offset on the
emulated stack and goto the target, returns pop it and re-enter the segment's
dispatch switch.  Far transfers set CS:IP and return to the run loop in
runtime.c, which picks the segment function (or a BIOS / sound-driver HLE).

Usage: python tools/recomp.py [data/GALAGA.EXE] > src/game.c
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone import x86_const as X

path = sys.argv[1] if len(sys.argv) > 1 else 'data/GALAGA.EXE'
raw = open(path, 'rb').read()
h = struct.unpack('<14H', raw[:28])
img = bytearray(raw[h[4] * 16:])
relocs = [struct.unpack('<HH', raw[h[12] + 4*i:h[12] + 4*i + 4]) for i in range(h[3])]
reloc_at = {s*16 + o for o, s in relocs}
entry = (h[11], h[10])
segvals = sorted({struct.unpack('<H', img[s*16+o:s*16+o+2])[0] for o, s in relocs})
code_segs = [s for s in segvals if s * 16 < len(img)]
DATA_SEG = code_segs[-1]          # Turbo Pascal puts DS right after the code
code_segs = code_segs[:-1]
seg_end = {s: e for s, e in zip(code_segs, code_segs[1:] + [DATA_SEG])}

md = Cs(CS_ARCH_X86, CS_MODE_16)
md.detail = True

# ---------------------------------------------------------------- decoding
def decode_at(seg, off):
    lin = seg*16 + off
    return next(md.disasm(bytes(img[lin:lin+16]), off), None)

seen = {}                 # (seg, off) -> insn
targets = set()           # (seg, off) reachable by transfer
todo = [entry]
main_end = seg_end[0] * 16
for i in range(main_end - 3):
    if img[i] == 0xe8:
        t = (i + 3 + struct.unpack('<h', img[i+1:i+3])[0]) & 0xffff
        if t + 3 < main_end and ((img[t] == 0xc8 and img[t+3] == 0) or img[t:t+3] == b'\x55\x89\xe5'):
            todo.append((0, t))
i = img.find(b'PSQRVW')
while 0 <= i < main_end:
    todo.append((0, i)); i = img.find(b'PSQRVW', i + 1)
for a in sys.argv[2:]:
    s, o = a.split(':'); todo.append((int(s, 16), int(o, 16)))

ENDS = {'ret', 'retf', 'iret', 'jmp', 'ljmp', 'hlt'}

def descend(todo):
    while todo:
        seg, off = todo.pop()
        targets.add((seg, off))
        while (seg, off) not in seen and seg*16 + off < seg_end.get(seg, DATA_SEG) * 16:
            insn = decode_at(seg, off)
            if insn is None:
                break
            seen[(seg, off)] = insn
            m = insn.mnemonic
            if m == 'lcall' and insn.bytes[0] == 0x9a:
                to, ts = struct.unpack('<HH', insn.bytes[1:5]); todo.append((ts, to))
            elif m == 'call' and insn.operands[0].type == X.X86_OP_IMM:
                todo.append((seg, insn.operands[0].imm & 0xffff))
            elif (m[0] == 'j' or m.startswith('loop')) and m != 'ljmp' and insn.operands[0].type == X.X86_OP_IMM:
                todo.append((seg, insn.operands[0].imm & 0xffff))
            if m in ENDS:
                break
            off += insn.size

descend(todo)
# Linear sweep over the gaps of each segment, so code reached only through
# runtime pointers (Turbo Pascal's text-file hooks, the odd unreferenced asm
# routine) still has a case to land on.  A sweep instruction that would
# overlap descended code is dropped.
covered = set()
for (seg, off), insn in seen.items():
    covered.update(range(seg*16 + off, seg*16 + off + insn.size))
swept = {}
for seg in code_segs:
    off, end = 0, (seg_end[seg] - seg) * 16
    while off < end:
        lin = seg*16 + off
        if lin in covered:
            off += 1; continue
        insn = decode_at(seg, off)
        if insn is None or any(a in covered for a in range(lin, lin + insn.size)):
            off += 1; continue
        swept[(seg, off)] = insn
        off += insn.size
allins = dict(swept); allins.update(seen)

# ---------------------------------------------------------------- C emission
R8 = {'al': 'AL', 'ah': 'AH', 'bl': 'BL', 'bh': 'BH', 'cl': 'CL', 'ch': 'CH', 'dl': 'DL', 'dh': 'DH'}
R16 = {'ax': 'AX', 'bx': 'BX', 'cx': 'CX', 'dx': 'DX', 'si': 'SI', 'di': 'DI', 'bp': 'BP', 'sp': 'SP',
       'cs': 'R.cs', 'ds': 'R.ds', 'es': 'R.es', 'ss': 'R.ss'}

class Unsupported(Exception):
    pass

def has_reloc(seg, insn):
    lin = seg*16 + insn.address
    return any(a in reloc_at for a in range(lin, lin + insn.size))

def memaddr(insn, op):
    m = op.mem
    parts = []
    base = insn.reg_name(m.base) if m.base else None
    index = insn.reg_name(m.index) if m.index else None
    if base: parts.append(R16[base])
    if index: parts.append(R16[index])
    if m.disp or not parts: parts.append(f'0x{m.disp & 0xffff:x}')
    seg = insn.reg_name(m.segment) if m.segment else ('ss' if base == 'bp' else 'ds')
    return R16[seg], '(uint16_t)(' + ' + '.join(parts) + ')'

class Op:
    """An operand as C: .get() -> rvalue, .set(v) -> statement."""
    def __init__(self, seg, insn, op, reloc=False):
        self.size = op.size
        self.kind = op.type
        if op.type == X.X86_OP_REG:
            n = insn.reg_name(op.reg)
            self.c = R8.get(n) or R16[n]
        elif op.type == X.X86_OP_IMM:
            v = op.imm & (0xff if op.size == 1 else 0xffff)
            self.c = f'0x{v:x}' + (' + LOADSEG' if reloc else '')
            if reloc: self.c = f'(uint16_t)({self.c})'
        elif op.type == X.X86_OP_MEM:
            self.seg, self.off = memaddr(insn, op)
        else:
            raise Unsupported('operand')
    def get(self):
        if self.kind == X.X86_OP_MEM:
            return f'{"rb" if self.size == 1 else "rw"}({self.seg}, {self.off})'
        return self.c
    def set(self, v):
        if self.kind == X.X86_OP_MEM:
            return f'{"wb" if self.size == 1 else "ww"}({self.seg}, {self.off}, {v});'
        return f'{self.c} = {v};'

ALU = {'add': 'add', 'adc': 'adc', 'sub': 'sub', 'sbb': 'sbb', 'and': 'and', 'or': 'or', 'xor': 'xor', 'cmp': 'sub', 'test': 'and'}
JCC = {'je': 'ZF', 'jne': '!ZF', 'jb': 'CF', 'jae': '!CF', 'jbe': '(CF || ZF)', 'ja': '(!CF && !ZF)',
       'jl': '(SF != OF)', 'jge': '(SF == OF)', 'jle': '(ZF || SF != OF)', 'jg': '(!ZF && SF == OF)',
       'js': 'SF', 'jns': '!SF', 'jo': 'OF', 'jno': '!OF', 'jp': 'PF', 'jnp': '!PF'}
SHIFTS = {'shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'}

def emit(seg, insn, labels):
    """C statements for one instruction (without its label)."""
    m = insn.mnemonic.split()[-1]       # capstone folds rep/repne into the mnemonic
    a = insn.address
    nxt = (a + insn.size) & 0xffff
    b0 = insn.bytes[0]
    ops = insn.operands
    rel = has_reloc(seg, insn)
    O = lambda i: Op(seg, insn, ops[i], rel and ops[i].type == X.X86_OP_IMM)
    W = lambda o: '' if o.size == 1 else '16'
    def jump(t, cond=None):
        t &= 0xffff
        back = t <= a
        poll = f'POLL(0x{a:x}); ' if back else ''
        go = f'goto L_{t:04x};' if (seg, t) in labels else f'{{ R.ip = 0x{t:x}; goto dispatch; }}'
        if cond:
            return f'if ({cond}) {{ {poll}{go} }}'
        return poll + go

    if m in ALU:
        d, s = O(0), O(1)
        f = ALU[m]
        sz = '8' if d.size == 1 else '16'
        if m in ('xor', 'sub') and d.kind == X.X86_OP_REG and s.kind == X.X86_OP_REG and d.c == s.c:
            return f'{d.c} = 0; flags_zero{sz}();'
        r = f'{f}{sz}({d.get()}, {s.get()})'
        if m in ('cmp', 'test'):
            return f'{r};'
        return d.set(r)
    if m in ('inc', 'dec', 'neg', 'not'):
        d = O(0); sz = '8' if d.size == 1 else '16'
        if m == 'not':
            return d.set(f'(uint{sz}_t)~{d.get()}')
        return d.set(f'{m}{sz}({d.get()})')
    if m == 'mov':
        d, s = O(0), O(1)
        return d.set(s.get())
    if m == 'xchg':
        d, s = O(0), O(1)
        return f'{{ uint16_t t_ = {d.get()}; {d.set(s.get())} {s.set("t_")} }}'
    if m == 'lea':
        d = O(0); _, off = memaddr(insn, ops[1])
        return d.set(off)
    if m in ('les', 'lds'):
        d = O(0); sg, off = memaddr(insn, ops[1])
        sreg = 'R.es' if m == 'les' else 'R.ds'
        return f'{{ uint16_t o_ = {off}, s_ = {sg}; {d.c} = rw(s_, o_); {sreg} = rw(s_, (uint16_t)(o_ + 2)); }}'
    if m == 'push':
        if b0 == 0x6a or b0 == 0x68 or ops[0].type == X.X86_OP_IMM:
            return f'push16({O(0).get()});' if b0 != 0x6a else f'push16((uint16_t)(int8_t)0x{ops[0].imm & 0xff:x});'
        return f'push16({O(0).get()});'
    if m == 'pop':
        d = O(0)
        return f'{{ uint16_t v_ = pop16(); {d.set("v_")} }}'
    if m == 'pushf': return 'push16(get_flags());'
    if m == 'popf': return f'set_flags(pop16()); POLL(0x{nxt:x});' if False else 'set_flags(pop16());'
    if m == 'enter':
        n, lvl = ops[0].imm & 0xffff, ops[1].imm & 0x1f
        if lvl:
            raise Unsupported('enter level')
        return f'push16(BP); BP = SP; SP -= 0x{n:x};'
    if m == 'leave': return 'SP = BP; BP = pop16();'
    if b0 == 0x98: return 'AX = (uint16_t)(int8_t)AL;'                       # cbw (capstone: cwde)
    if b0 == 0x99: return 'DX = (AX & 0x8000) ? 0xffff : 0;'                 # cwd (capstone: cdq)
    if m in ('clc', 'stc', 'cmc', 'cld', 'std', 'cli', 'sti'):
        return {'clc': 'CF = 0;', 'stc': 'CF = 1;', 'cmc': 'CF = !CF;', 'cld': 'DF = 0;', 'std': 'DF = 1;',
                'cli': 'IF = 0;', 'sti': 'IF = 1;'}[m]
    if m == 'nop': return ';'
    if m in SHIFTS:
        d = O(0); sz = '8' if d.size == 1 else '16'
        cnt = O(1).get() if len(ops) > 1 else '1'
        return d.set(f'{m if m != "sal" else "shl"}{sz}({d.get()}, {cnt})')
    if m in ('mul', 'imul', 'div', 'idiv') and len(ops) == 1:
        s = O(0); sz = '8' if s.size == 1 else '16'
        return f'{m}{sz}({s.get()}, 0x{a:x});'
    if m == 'imul' and len(ops) == 3:
        d, s, i = O(0), O(1), O(2)
        return d.set(f'imul3({s.get()}, {i.get()})')
    if m == 'imul' and len(ops) == 2:
        d, s = O(0), O(1)
        return d.set(f'imul3({d.get()}, {s.get()})')
    if m in JCC:
        return jump(ops[0].imm, JCC[m])
    if m == 'jcxz':
        return jump(ops[0].imm, '!CX')
    if m == 'loop':
        return f'if (--CX) {{ {jump(ops[0].imm)} }}'
    if m == 'jmp':
        if ops[0].type == X.X86_OP_IMM:
            return jump(ops[0].imm)
        o = O(0)
        return f'R.ip = {o.get()}; goto dispatch;'
    if m == 'call':
        if ops[0].type == X.X86_OP_IMM:
            t = ops[0].imm & 0xffff
            go = f'goto L_{t:04x};' if (seg, t) in labels else f'{{ R.ip = 0x{t:x}; goto dispatch; }}'
            return f'push16(0x{nxt:x}); {go}'
        o = O(0)
        return f'{{ uint16_t t_ = {o.get()}; push16(0x{nxt:x}); R.ip = t_; goto dispatch; }}'
    if m == 'lcall':
        if b0 == 0x9a:
            to, ts = struct.unpack('<HH', insn.bytes[1:5])
            return f'push16(R.cs); push16(0x{nxt:x}); R.cs = 0x{ts:x} + LOADSEG; R.ip = 0x{to:x}; return;'
        sg, off = memaddr(insn, ops[0])
        return (f'{{ uint16_t o_ = {off}, s_ = {sg}; uint16_t ti_ = rw(s_, o_), ts_ = rw(s_, (uint16_t)(o_ + 2)); '
                f'push16(R.cs); push16(0x{nxt:x}); R.cs = ts_; R.ip = ti_; return; }}')
    if m == 'ljmp':
        if b0 == 0xea:
            to, ts = struct.unpack('<HH', insn.bytes[1:5])
            return f'R.cs = 0x{ts:x} + LOADSEG; R.ip = 0x{to:x}; return;'
        sg, off = memaddr(insn, ops[0])
        return f'{{ uint16_t o_ = {off}, s_ = {sg}; R.ip = rw(s_, o_); R.cs = rw(s_, (uint16_t)(o_ + 2)); return; }}'
    if m == 'ret':
        n = ops[0].imm if ops else 0
        return f'R.ip = pop16(); SP += 0x{n:x}; goto dispatch;'
    if m == 'retf':
        n = ops[0].imm if ops else 0
        return f'R.ip = pop16(); R.cs = pop16(); SP += 0x{n:x}; return;'
    if m == 'iret':
        return 'do_iret(); return;'
    if m == 'int':
        return f'R.ip = 0x{nxt:x}; if (do_int(0x{ops[0].imm & 0xff:x})) return;'
    if m == 'in':
        d = O(0); port = O(1).get()
        return f'POLL(0x{a:x}); ' + d.set(f'port_in{W(d)}({port})')
    if m == 'out':
        p, v = O(0), O(1)
        return f'port_out{W(v)}({p.get()}, {v.get()});'
    # string ops
    if m in ('movsb', 'movsw', 'stosb', 'stosw', 'lodsb', 'lodsw', 'scasb', 'scasw', 'cmpsb', 'cmpsw'):
        w = m[-1]
        src_seg = 'R.ds'
        for o in ops:
            if o.type == X.X86_OP_MEM and insn.reg_name(o.mem.base) == 'si' and o.mem.segment:
                src_seg = R16[insn.reg_name(o.mem.segment)]
        rep = ''
        if insn.prefix[0] == 0xf3: rep = 'rep'
        if insn.prefix[0] == 0xf2: rep = 'repne'
        if rep == 'rep' and m[:4] in ('scas', 'cmps'): rep = 'repe'
        return f'str_{m[:4]}_{w}({src_seg}, {rep or "0"});'.replace('(R.ds, 0)', '(R.ds, NOREP)').replace(', 0)', ', NOREP)')
    if m == 'hlt':
        return f'POLL(0x{a:x}); R.ip = 0x{nxt:x}; goto dispatch;'
    raise Unsupported(m)

def gen():
    labels = set(allins)
    call_targets = set()
    for (sg, o), ins in seen.items():
        if ins.mnemonic == 'call' and ins.operands[0].type == X.X86_OP_IMM:
            call_targets.add((sg, ins.operands[0].imm & 0xffff))
        if ins.mnemonic == 'lcall' and ins.bytes[0] == 0x9a:
            to, ts = struct.unpack('<HH', ins.bytes[1:5]); call_targets.add((ts, to))
    out = []
    w = out.append
    w('/* Generated by tools/recomp.py from GALAGA.EXE -- do not edit. */')
    w('#include "cpu.h"')
    w(f'const uint16_t entry_cs = 0x{entry[0]:x}, entry_ip = 0x{entry[1]:x};')
    w(f'const uint16_t exe_ss = 0x{h[7]:x}, exe_sp = 0x{h[8]:x}, exe_data_seg = 0x{DATA_SEG:x};')
    bad = 0
    for seg in code_segs:
        w(f'\nvoid seg_{seg:04x}(void)\n{{\ndispatch:\n  switch (R.ip) {{')
        offs = sorted(o for (s, o) in allins if s == seg)
        prev_end = None
        prev_falls = False
        for off in offs:
            insn = allins[(seg, off)]
            if prev_falls and prev_end != off:
                w(f'    R.ip = 0x{prev_end:x}; goto dispatch;')
            try:
                body = emit(seg, insn, labels)
            except (Unsupported, KeyError, IndexError, AttributeError) as e:
                body = f'bad_insn(0x{off:x}, "{insn.mnemonic} {insn.op_str}"); return;'
                if (seg, off) in seen:
                    bad += 1
                    print(f'unsupported {seg:04x}:{off:04x} {insn.mnemonic} {insn.op_str}', file=sys.stderr)
            src = 'S' if (seg, off) in swept else ' '
            w(f'  case 0x{off:x}: L_{off:04x}: /* {src}{insn.mnemonic} {insn.op_str} */')
            if (seg, off) in call_targets:
                w(f'    TRACE_SUB(0x{seg:x}, 0x{off:x});')
            w(f'    R.ip = 0x{off:x}; {body}' if 'POLL' in body or 'bad_insn' in body or 'mul' in body or 'div' in body else f'    {body}')
            prev_end = (off + insn.size) & 0xffff
            prev_falls = insn.mnemonic not in ENDS
        if prev_falls:
            w(f'    R.ip = 0x{prev_end:x}; goto dispatch;')
        w(f'  default: bad_target(); return;\n  }}\n}}')
    w('\nvoid run_segment(uint16_t seg)\n{\n  switch (seg) {')
    for seg in code_segs:
        w(f'  case 0x{seg:x}: seg_{seg:04x}(); return;')
    w('  default: hle_far(); return;\n  }\n}')
    w('const uint16_t code_segs[] = {' + ', '.join(f'0x{s:x}' for s in code_segs) + '};')
    w(f'const int num_code_segs = {len(code_segs)};')
    print('\n'.join(out))
    print(f'segments {[hex(s) for s in code_segs]} data {DATA_SEG:#x}; descended {len(seen)} swept {len(swept)} '
          f'unsupported(descended) {bad}', file=sys.stderr)

gen()
