#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Device memory bytes and instructions per step of a kangaroo kernel with private arrays (kang64 .. kang256c), from the SASS.

    tools/research/kang_bytes.py --nvrtc LIBNVRTC --ptxas PTXAS --nvdisasm NVDISASM [--module kang128c] [--arch sm_86] [-DNAME=... ]

The module (src/kernels/ptx/modules.txt; or `kang` with -D options for a build that is not a module) is compiled with NVRTC and ptxas as
tools/gen_ptx.py does, disassembled, and the executed path of one turn of the loop over the walkers of a round is rebuilt from the SASS as
kang_block_sched.py does for the block herd: the loop is the one whose body stores the history entries (STG.U16), a conditional branch is
taken when it skips a block that holds an ATOMG (a restart, a distinguished point) and not taken when its target calls kang_emit_dp; the
other conditional branches are not taken (the cycle update then counts its checkpoint form, a few instructions more than the common one).
The private arrays of the work-item live in device memory, so every LDL and STL of the path is traffic to it (a predicated store and its
complement under the other polarity of the same predicate count once): the script prints, per step, the bytes read and written by the local
loads and stores by purpose (from their offsets in the stack frame, which the kernel lays out as X, Y, TP, FL, PV, P2, CM, LD), the history
store, the jump-table loads (served by L1 and L2: the table is 21 KiB), and the instructions of the turn on the two integer pipes and the
rest; then the round overhead outside that loop (the check of the product, fe_inv as executed for a warp: kang_block_sched.inv_path) per
step. Used for docs/en/RESEARCH.md section 5 (after 1.19.2). The tools: see tools/research/README.md (sass_count.py)."""
import collections, importlib.util, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('kang_block_sched', os.path.join(HERE, 'kang_block_sched.py'))
kbs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kbs)


def width(op):
    return 16 if '.128' in op else 8 if '.64' in op else 2 if '.U16' in op else 1 if '.U8' in op else 4


def walker_loop(ins, lab):
    """(head, back): the loop whose body stores the history entries and reads x (not the one that fills the history with H_NOP after a
    zero product, which may reload a spilled register)"""
    best = None
    for i, (_, t, _) in enumerate(ins):
        m = re.match(r'(?:@!?P\d\s+)?BRA `\(\.(L_x_\d+)\)', t)       # the back edge is conditional on some targets (sm_75)
        if m and lab[m.group(1)] < i:
            h = lab[m.group(1)]
            body = ins[h:i]
            if any('STG.E.U16' in x[1] for x in body) and any('LDL.128' in x[1] for x in body) and (best is None or i - h < best[1] - best[0]):
                best = (h, i)
    if best is None:
        raise SystemExit('no loop over the walkers found (a block herd kernel? use kang_block_sched.py)')
    return best


def path_of(ins, lab, head, back):
    i, path, idx = head, [], []
    while True:
        off, t, c = ins[i]
        path.append(ins[i])
        idx.append(i)
        m = re.match(r'(@!?P\d\s+)?(BRA|CALL\.REL\.NOINC)\s+`\(([.$]?[\w$]+)\)', t)
        if m:
            cond, kind, tgt = m.group(1), m.group(2), m.group(3).lstrip('.')
            if kind == 'CALL.REL.NOINC':
                if not tgt.startswith('L_x'):
                    if 'emit_dp' in tgt or 'fe_inv' in tgt:
                        raise SystemExit('unexpected call on the common path at %x: %s' % (off, t))
                    i += 1
                    continue
                taken = False                       # a call to a label: the exit of the loop
            elif not cond or lab[tgt] == head:
                taken = True                        # unconditional, or the back edge of the loop (conditional on sm_75)
            else:
                j = lab[tgt]
                skipped = ins[i + 1:j] if j > i else []
                if any('emit_dp' in x[1] for x in ins[j:kbs.block_end(ins, j) + 1]):
                    taken = False
                else:
                    taken = any('ATOMG' in x[1] for x in skipped)
            if taken:
                if lab[tgt] == head:
                    break
                i = lab[tgt]
                continue
        i += 1
        if i > back:
            break
    return path, idx


def frame_base(ins, i, reg, depth=0):
    """the constant offset from the stack frame (R1) that register `reg` holds at instruction i of the kernel, or None: the last definition
    of `reg` before i is followed through adds of immediates and scaled indices (k * 32 + base), at most three deep"""
    if reg == 'R1':
        return 0
    if depth > 3:
        return None
    for j in range(i - 1, -1, -1):
        t = ins[j][1]
        m = re.match(r'(?:@!?P\d\s+)?(\S+)\s+(\S+),\s*(.*)', t)
        if not m or m.group(2) != reg:
            continue
        op, ops = m.group(1), [x.strip().replace('.reuse', '') for x in m.group(3).split(',')]
        if op in ('IMAD', 'IMAD.U32') and len(ops) == 3 and ops[1].startswith('0x'):
            return frame_base(ins, j, ops[2], depth + 1)                 # k * scale + base
        if op == 'LEA' and len(ops) == 3:
            return frame_base(ins, j, ops[1], depth + 1)
        if op == 'IMAD.IADD' and len(ops) == 3:
            a, b = frame_base(ins, j, ops[0], depth + 1), frame_base(ins, j, ops[2], depth + 1)
            return a if b is None else b if a is None else a + b
        if op in ('IADD3', 'UIADD3') and 3 <= len(ops) <= 4:
            vals = []
            for x in ops[len(ops) - 3:]:                                  # a four-operand form writes its carry into a predicate first
                if x.startswith('0x'):
                    vals.append(int(x, 16))
                elif x in ('RZ', 'URZ'):
                    vals.append(0)
                else:
                    v = frame_base(ins, j, x, depth + 1)
                    if v is None:
                        return None
                    vals.append(v)
            return sum(vals)
        if op in ('MOV', 'IMAD.MOV.U32', 'R2UR', 'UMOV') and ops[-1][0] in 'RU':
            return frame_base(ins, j, ops[-1], depth + 1)
        if op == 'IMAD.SHL.U32':
            return 0                                                      # an index alone (k * scale, the set of the TP products)
        if op == 'LOP3.LUT' and ops[1].startswith('0x'):
            return frame_base(ins, j, ops[0], depth + 1)                  # the set index toggled: the base is the register operand
        if os.environ.get('KANG_BYTES_DEBUG'):
            print('  unresolved base: %s' % t)
        return None
    return None


def layout(nk, mc):
    """the private arrays of pz_kang_step in the order the kernel declares them: [(name, start, end)]"""
    sizes = [('x', 32 * nk), ('y', 32 * nk), ('TP (batch products)', (2 * nk // mc if mc > 1 else nk) * 32),
             ('FL (flags, jump index)', 4 * nk), ('PV (tag one step back)', 4 * nk), ('P2 (tag two steps back)', 4 * nk),
             ('CM (checkpoint, least tag)', 8 * nk), ('LD (step of the last DP)', 8 * nk)]
    out, at = [], 0
    for name, sz in sizes:
        out.append((name, at, at + sz))
        at += sz
    return out


def mem_bytes(ins, path_idx, arrays):
    """{(kind, purpose): bytes} of the local loads and stores of the path (purpose from the offset in the stack frame), the history stores
    and the jump table loads. Predicated stores: both polarities of a predicate count once."""
    out = collections.Counter()
    seen = {}
    for i in path_idx:
        t = ins[i][1]
        m = re.match(r'(@(!?)(P\d)\s+)?(LDL|STL|LDG|STG)(\S*)\s+(?:\S+,\s+)?\[(\S+?)\]', t)
        if not m:
            continue
        pred, neg, preg, base, suf, addr = m.groups()
        w = width(base + suf)
        if base in ('LDG', 'STG'):
            out[('L1' if base == 'LDG' else 'write', 'jump table' if base == 'LDG' else 'history')] += w
            continue
        mo = re.match(r'(R\d+)(?:\+0x([0-9a-f]+))?', addr)
        offs = (int(mo.group(2), 16) if mo.group(2) else 0)
        fb = frame_base(ins, i, mo.group(1))
        purpose = 'unknown (offset 0x%x from a register)' % offs
        if fb is not None:
            offs += fb
            purpose = 'spilled registers (past the arrays)'
            for name, lo, hi in arrays:
                if lo <= offs < hi:
                    purpose = name
        kind = 'read' if base == 'LDL' else 'write'
        if pred:
            key = (preg, base, w)
            if key in seen and seen[key] != neg:
                continue                            # the other polarity of a pair: counted once
            seen[key] = neg
            if not any(re.match(r'@%s%s\s+%s%s\s' % ('' if neg else '!', preg, base, re.escape(suf)), ins[j][1]) for j in path_idx):
                kind += ' (conditional)'           # no store under the other polarity: a rare event's store (a checkpoint, a restart)
        out[(kind, purpose)] += w
    return out


def main():
    args = sys.argv[1:]
    opt = lambda name, dflt=None: args[args.index(name) + 1] if name in args else dflt
    nvrtc, ptxas, nvdisasm = opt('--nvrtc'), opt('--ptxas'), opt('--nvdisasm')
    if not (nvrtc and ptxas and nvdisasm):
        raise SystemExit(__doc__)
    module, arch = opt('--module', 'kang128c'), opt('--arch', 'sm_86')
    defines = [a for a in args if a.startswith('-D')]
    text = compile_module(nvrtc, ptxas, nvdisasm, module, arch, defines)
    funcs, labels = kbs.parse(text)
    ins = funcs['pz_kang_step']
    lab = {k: v[1] for k, v in labels.items() if v[0] == 'pz_kang_step'}
    head, back = walker_loop(ins, lab)
    path, idx = path_of(ins, lab, head, back)
    m = re.match(r'kang(\d+)([a-z]?)', module)
    nk, mc = (int(m.group(1)), {'b': 2, 'c': 4, 'd': 8}.get(m.group(2), 1)) if m else (0, 1)
    for d in defines:                                   # a build that is not a module: `kang` with -DNK= and -DMC=
        if d.startswith('-DNK='):
            nk = int(d[5:])
        if d.startswith('-DMC='):
            mc = int(d[5:])
    steps = sum('STG.E.U16' in x[1] for x in path)
    n, a, f, o, _ = kbs.stats(path)
    print('%s %s: loop over the walkers at %x..%x, %d steps per turn' % (module, arch, ins[head][0], ins[back][0], steps))
    print('  per step: %.1f instructions (ALU pipe %.1f, FMA pipe %.1f, other %.1f)' % (n / steps, a / steps, f / steps, o / steps))
    mb = mem_bytes(ins, idx, layout(nk, mc))
    tot = collections.Counter()
    for (kind, purpose), b in sorted(mb.items()):
        print('  %-6s %-40s %6.1f bytes per step' % (kind, purpose, b / steps))
        tot[kind] += b
    print('  device memory per step: %.1f read + %.1f written = %.1f, plus %.1f written on rare events; jump table through L1: %.1f' %
          (tot['read'] / steps, tot['write'] / steps, (tot['read'] + tot['write']) / steps, tot['write (conditional)'] / steps, tot['L1'] / steps))
    # the round outside the loop over the walkers: from the head of the round loop (the enclosing backward branch) to the walker loop, and
    # from the end of the walker loop to the round's back edge, plus fe_inv as executed by a warp
    outer = None
    for i, (_, t, _) in enumerate(ins):
        m = re.match(r'(?:@!?P\d\s+)?BRA `\(\.(L_x_\d+)\)', t)
        if m and lab[m.group(1)] < head and i > back and (outer is None or i - lab[m.group(1)] < outer[1] - outer[0]):
            outer = (lab[m.group(1)], i)
    if outer:
        rest = ins[outer[0]:head] + ins[back + 1:outer[1] + 1]
        rest = [x for x in rest if 'ATOMG' not in x[1]]
        inv, _ = kbs.inv_path(funcs, labels)
        rn, ra, rf, ro, _ = kbs.stats(rest)
        ni, ia, fi, oi, _ = kbs.stats(inv)
        print('  round overhead (static, outside the loop, with the rare blocks): %d instructions; fe_inv as executed per warp: %d (ALU %d, FMA %d)' % (rn, ni, ia, fi))
        print('  with NK walkers per work-item, per step: (%d + %d) / NK' % (rn, ni))


def compile_module(nvrtc, ptxas, nvdisasm, module, arch, defines):
    import ctypes, subprocess, tempfile
    spec = importlib.util.spec_from_file_location('gen_ptx', os.path.join(kbs.ROOT, 'tools', 'gen_ptx.py'))
    gp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gp)
    mods = {m[0]: m for m in gp.read_modules()}
    if module in mods:
        name, files, defs = mods[module]
    elif module == 'kang':
        name, files, defs = module, gp.SOURCES['kang'], []
    else:
        raise SystemExit('unknown module %s (see src/kernels/ptx/modules.txt)' % module)
    ptx = gp.compile_one(ctypes.CDLL(nvrtc), name, files, defs + defines)
    with tempfile.TemporaryDirectory() as d:
        open(d + '/k.ptx', 'w').write(ptx)
        r = subprocess.run([ptxas, '-arch=' + arch, '-v', '-o', d + '/k.cubin', d + '/k.ptx'], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('ptxas:\n' + r.stderr)
        for line in r.stderr.splitlines():
            if 'registers' in line or ('spill' in line and '0 bytes spill' not in line):
                print('  ' + line.split('info    : ')[-1])
        return subprocess.run([nvdisasm, '-hex', '-c', d + '/k.cubin'], capture_output=True, text=True).stdout


if __name__ == '__main__':
    main()
