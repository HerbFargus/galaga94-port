"""Recursive-descent disassembly of GALAGA.EXE (16-bit real mode, Turbo Pascal 6).

Writes out/galaga.asm: one listing for all code segments, with labels for every
call/jump target found by following code from the entry point, every far-call
target, and every near call whose target starts with a procedure prologue.
Usage: python tools/disasm.py [data/GALAGA.EXE]
"""
import struct, sys, os
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

path = sys.argv[1] if len(sys.argv) > 1 else 'data/GALAGA.EXE'
raw = open(path, 'rb').read()
h = struct.unpack('<14H', raw[:28])
hdr = h[4] * 16
img = bytearray(raw[hdr:])
relocs = [struct.unpack('<HH', raw[h[12] + 4*i:h[12] + 4*i + 4]) for i in range(h[3])]
reloc_at = {s*16 + o for o, s in relocs}
entry = (h[11], h[10])

segvals = sorted({struct.unpack('<H', img[s*16+o:s*16+o+2])[0] for o, s in relocs})
code_segs = [s for s in segvals if s * 16 < len(img)]
md = Cs(CS_ARCH_X86, CS_MODE_16)

todo = [entry]
labels = {entry[0]*16 + entry[1]: 'entry'}
# Seeds: near calls (E8) in the main segment whose target starts with a 286
# ENTER or push bp/mov bp,sp.  String constants live in the code segment and
# break the descent, so some callers are never reached from the entry point.
main_end = code_segs[1] * 16
for i in range(main_end - 3):
    if img[i] == 0xe8:
        t = (i + 3 + struct.unpack('<h', img[i+1:i+3])[0]) & 0xffff
        if t + 3 < main_end and ((img[t] == 0xc8 and img[t+3] == 0) or img[t:t+3] == b'\x55\x89\xe5'):
            todo.append((0, t)); labels.setdefault(t, 'near')
# Hand-written asm procedures open with push ax,bx,cx,dx,si,di (PSQRVW).
i = img.find(b'PSQRVW')
while 0 <= i < main_end:
    todo.append((0, i)); labels.setdefault(i, 'near')
    i = img.find(b'PSQRVW', i + 1)
for extra in sys.argv[2:]:
    t = int(extra, 16); todo.append((0, t)); labels.setdefault(t, 'near')

seen = {}
while todo:
    seg, off = todo.pop()
    lin = seg*16 + off
    while lin not in seen and lin < len(img):
        insn = next(md.disasm(bytes(img[lin:lin+16]), off), None)
        if insn is None:
            break
        seen[lin] = (seg, insn)
        m, ops = insn.mnemonic, insn.op_str
        if m == 'lcall' and insn.bytes[0] == 0x9a:
            to, ts = struct.unpack('<HH', insn.bytes[1:5])
            labels.setdefault(ts*16+to, 'far'); todo.append((ts, to))
        elif m == 'call' and ops.startswith('0x'):
            t = int(ops, 16); labels.setdefault(seg*16+t, 'near'); todo.append((seg, t))
        elif (m.startswith('j') or m.startswith('loop')) and ops.startswith('0x'):
            t = int(ops, 16); labels.setdefault(seg*16+t, 'loc'); todo.append((seg, t))
        elif m == 'ljmp' and insn.bytes[0] == 0xea:
            to, ts = struct.unpack('<HH', insn.bytes[1:5]); labels.setdefault(ts*16+to, 'far'); todo.append((ts, to))
        if m in ('ret', 'retf', 'iret', 'jmp', 'ljmp', 'hlt'):
            break
        off += insn.size; lin = seg*16 + off

os.makedirs('out', exist_ok=True)
covered = set()
for lin, (seg, insn) in seen.items():
    covered.update(range(lin, lin + insn.size))
with open('out/galaga.asm', 'w') as f:
    f.write(f'; {path}: entry {entry[0]:04x}:{entry[1]:04x} ss:sp {h[7]:04x}:{h[8]:04x}\n')
    f.write(f'; segments {[hex(s) for s in code_segs]}  decoded bytes {len(covered)}\n')
    prev = None
    for lin in sorted(seen):
        seg, insn = seen[lin]
        if prev is not None and lin != prev:
            gap = bytes(img[prev:lin])
            txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in gap[:64])
            f.write(f'\n; ---- gap {prev:05x}..{lin:05x} ({lin-prev} bytes) {txt}\n')
        if lin in labels:
            k = labels[lin]
            name = 'sub' if k in ('near', 'far', 'entry') else 'loc'
            f.write(('\n' if name == 'sub' else '') + f'{name}_{seg:04x}_{insn.address:04x}:  ; {k}\n')
        rel = ' ; RELOC' if any(a in reloc_at for a in range(lin, lin+insn.size)) else ''
        f.write(f'  {seg:04x}:{insn.address:04x}  {insn.bytes.hex():<14} {insn.mnemonic} {insn.op_str}{rel}\n')
        prev = lin + insn.size
print('instructions', len(seen), 'bytes', len(covered), 'subs', sum(1 for k in labels.values() if k != 'loc'))
for s, e in zip(code_segs, code_segs[1:] + [len(img)//16]):
    c = sum(1 for a in covered if s*16 <= a < e*16)
    print(f'seg {s:04x}: {c}/{(e-s)*16} bytes decoded')
