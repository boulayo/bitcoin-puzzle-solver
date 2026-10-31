#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Executed counts and an issue model of one SM running the block-herd kangaroo kernel (kang4g), from the SASS that NVIDIA's compilers produce.

    tools/research/kang_block_sched.py --nvrtc LIBNVRTC --ptxas PTXAS --nvdisasm NVDISASM [--module kang4g] [--arch sm_86] [--rounds 6]

The module (from src/kernels/ptx/modules.txt) is compiled with NVRTC and ptxas as tools/gen_ptx.py does and disassembled with the encodings
(nvdisasm -hex), and the control words that ptxas writes into every instruction are read: the stall count (the cycles before the same warp may
issue again: the fixed latencies of the dependencies), and the scoreboard barriers of the loads and stores. The executed path of one round of
a work-item is rebuilt from the loop of the kernel that holds the two BAR.SYNC: the rare blocks are skipped (a conditional branch is taken when
it jumps over a block of fewer than 400 instructions that holds an ATOMG, and not taken when its target block calls kang_emit_dp or when it
jumps over the walkers' code itself, the branch on the work-items past the end of the herd), and for the warp whose turn it is to invert, the
part between the barriers with fe_inv expanded: 19 rounds of its divstep loop, 12.6 turns of the inner loop each on average (the slowest of
32 lanes, RESEARCH.md section 5 after 1.13.0). It prints the instructions per round (ALU pipe: IADD3, LOP3, SHF, ...; FMA pipe: IMAD in all its
forms; other), the sum of the stall counts (the time of one warp alone), and the cycles per round of an SM with one or two resident blocks:
four warps each, warp w of a block on partition w, one instruction issued per cycle and partition, a warp instruction holding its 16-lane pipe
two cycles (IMAD.WIDE 2 x the given slots), loads from L1 and the local loads and stores at 35 cycles, shared memory at 25, the barriers of
each block. Used for docs/en/RESEARCH.md section 5 (after 1.19.1). The tools: see tools/research/README.md (sass_count.py)."""
import collections, ctypes, importlib.util, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ALU = {'IADD3', 'LOP3', 'SHF', 'LEA', 'SEL', 'PRMT', 'ISETP', 'FLO', 'POPC', 'BREV', 'MOV', 'IMNMX', 'PLOP3', 'P2R', 'R2P', 'BMSK', 'VOTE', 'IABS',
       'SGXT'}


def opname(t):
    return re.sub(r'^@!?U?P\w+\s+', '', t).split()[0]


def pipe(op):
    b = op.split('.')[0]
    return 'F' if b == 'IMAD' else 'A' if b in ALU else 'O'


def compile_module(nvrtc, ptxas, nvdisasm, module, arch):
    spec = importlib.util.spec_from_file_location('gen_ptx', os.path.join(ROOT, 'tools', 'gen_ptx.py'))
    gp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gp)
    mods = {m[0]: m for m in gp.read_modules()}
    if module not in mods:
        raise SystemExit('unknown module %s (see src/kernels/ptx/modules.txt)' % module)
    name, files, defines = mods[module]
    ptx = gp.compile_one(ctypes.CDLL(nvrtc), name, files, defines)
    with tempfile.TemporaryDirectory() as d:
        open(d + '/k.ptx', 'w').write(ptx)
        r = subprocess.run([ptxas, '-arch=' + arch, '-v', '-o', d + '/k.cubin', d + '/k.ptx'], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('ptxas:\n' + r.stderr)
        for line in r.stderr.splitlines():
            if 'registers' in line or 'spill' in line:
                print('  ' + line.split('info    : ')[-1])
        return subprocess.run([nvdisasm, '-hex', '-c', d + '/k.cubin'], capture_output=True, text=True).stdout


def parse(text):
    """{function: [(offset, text, (stall, yield, write barrier, read barrier, wait mask))]} and {label: (function, index)}"""
    funcs, labels, cur, pend = collections.OrderedDict(), {}, None, None
    for l in text.splitlines():
        m = re.match(r'\s*\.text\.(\S+):', l) or re.match(r'^(\$\S+):', l)
        if m:
            cur = m.group(1).split('$')[-1]
            funcs[cur] = []
            continue
        m = re.match(r'\s*\.(L_x_\d+):', l)
        if m and cur is not None:
            labels[m.group(1)] = (cur, len(funcs[cur]))
            continue
        m = re.match(r'\s*/\*([0-9a-f]{4,})\*/\s+(.*?)\s*;\s*/\*\s*0x([0-9a-f]{16})\s*\*/', l)
        if m and cur is not None:
            pend = (int(m.group(1), 16), m.group(2).strip())
            continue
        m = re.match(r'\s*/\*\s*0x([0-9a-f]{16})\s*\*/\s*$', l)
        if m and pend is not None:
            c = int(m.group(1), 16) >> 41
            funcs[cur].append(pend + ((c & 0xf, (c >> 4) & 1, (c >> 5) & 7, (c >> 8) & 7, (c >> 11) & 0x3f),))
            pend = None
    return funcs, labels


def block_end(ins, i):
    while i < len(ins) and not re.search(r'\bBRA\b|EXIT|RET', ins[i][1]):
        i += 1
    return i


def round_path(funcs, labels, inverter):
    """the executed instructions of one round of the main loop, for the inverting warp or for the others"""
    name = 'pz_kang_step'
    ins = funcs[name]
    lab = {k: v[1] for k, v in labels.items() if v[0] == name}
    start = None
    for i, (_, t, _) in enumerate(ins):
        m = re.match(r'BRA `\(\.(L_x_\d+)\)', t)
        if m and lab[m.group(1)] < i and any('BAR.SYNC' in x[1] for x in ins[lab[m.group(1)]:i]):
            start = lab[m.group(1)]
    i, path, nbar, seen_inv = start, [], 0, False
    while True:
        off, t, c = ins[i]
        path.append(ins[i])
        nbar += 'BAR.SYNC' in t
        m = re.match(r'(@!?P\d\s+)?(BRA|CALL\.REL\.NOINC)\s+`\(([.$]?[\w$]+)\)', t)
        if m:
            cond, kind, tgt = m.group(1), m.group(2), m.group(3).lstrip('.')
            if kind == 'CALL.REL.NOINC':
                if not cond and not tgt.startswith('L_x'):
                    if 'fe_inv' not in tgt:
                        raise SystemExit('unexpected call on the common path at %x: %s' % (off, t))
                    i += 1
                    continue
                taken = False                       # a call to a label: the exit of the loop
            elif not cond:
                taken = True
            elif nbar == 1 and not seen_inv:        # the first test after the first barrier: is it this warp's turn to invert
                taken, seen_inv = not inverter, True
            else:
                j = lab[tgt]
                skipped = ins[i + 1:j] if j > i else []
                if any('emit_dp' in x[1] for x in ins[j:block_end(ins, j) + 1]):
                    taken = False
                elif len(skipped) > 400:            # the walkers' code, skipped by a work-item past the end of the herd
                    taken = False
                else:
                    taken = any('ATOMG' in x[1] for x in skipped)
            if taken:
                if lab[tgt] == start:
                    break
                i = lab[tgt]
                continue
        i += 1
        if i == start:
            break
    return path


def inv_path(funcs, labels, rounds=19, turns=(13,) * 12 + (12,) * 7):
    """the executed instructions of fe_inv for one warp: the prologue, `rounds` rounds of the divstep loop (`turns` turns of the inner loop each)
    and the epilogue up to the return; and the parts, for the counts"""
    f = [n for n in funcs if 'fe_inv' in n][0]
    ins = funcs[f]
    lab = {k: v[1] for k, v in labels.items() if v[0] == f}
    back = []
    for i, (_, t, _) in enumerate(ins):
        m = re.search(r'BRA.*`\(\.(L_x_\d+)\)', t)
        if m and lab[m.group(1)] < i:
            back.append((lab[m.group(1)], i))
    outer = max(back, key=lambda x: x[1] - x[0])
    inner = min(back, key=lambda x: x[1] - x[0])
    pro, pre, turn, post = ins[:outer[0]], ins[outer[0]:inner[0]], ins[inner[0]:inner[1] + 1], ins[inner[1] + 1:outer[1] + 1]
    epi = []
    for x in ins[outer[1] + 1:]:
        epi.append(x)
        if 'RET' in x[1]:
            break
    tr = list(pro)
    for r in range(rounds):
        tr += pre + turn * turns[r % len(turns)] + post
    return tr + epi, (('prologue', pro), ('round, before the turns', pre), ('turn', turn), ('round, after the turns', post), ('epilogue', epi))


def stats(seq):
    pa = collections.Counter(pipe(opname(t)) for _, t, _ in seq)
    return len(seq), pa['A'], pa['F'], pa['O'], sum(c[0] for _, _, c in seq)


def split(seq):
    b = [k for k, x in enumerate(seq) if 'BAR.SYNC' in x[1]]
    return seq[:b[0] + 1], seq[b[0] + 1:b[1] + 1], seq[b[1] + 1:]


def simulate(parts, grps, rounds, wide, mem=35, lds=25):
    """cycles per round of an SM with one block per entry of grps (its block index: the inverting warp of round r is (r + index) mod 4)"""
    def cls(seq):
        out = []
        for _, t, (stall, _, wr, rd, wait) in seq:
            op = opname(t)
            p, base = pipe(op), op.split('.')[0]
            lat = lds if base in ('LDS', 'STS') else mem if base in ('LDL', 'STL', 'LDG', 'LDC', 'LD', 'ULDC', 'ATOMG') else 20
            out.append((p, 2 * wide if op.startswith('IMAD.WIDE') else 2 if p in 'AF' else 1, max(stall, 1), wait, wr, rd, lat, 'BAR.SYNC' in t))
        return out
    A, M, MO, C = (cls(x) for x in parts)
    W = [dict(b=b, w=w, g=g, r=0, pc=0, ready=0, bars=[0] * 6, at_bar=False, last=-1) for b, g in enumerate(grps) for w in range(4)]

    def load(x):
        x['seq'] = A + (M if (x['r'] + x['g']) % 4 == x['w'] else MO) + C
        x['pc'] = 0
    for x in W:
        load(x)
    free = [{'A': 0, 'F': 0, 'O': 0} for _ in range(4)]
    waiting = [0] * len(grps)
    t, t0 = 0, None
    while min(x['r'] for x in W) < rounds:
        for p in range(4):
            for x in sorted((x for x in W if x['w'] == p and x['r'] < rounds and not x['at_bar']), key=lambda x: x['last']):
                pp, occ, stall, wait, wr, rd, lat, bar = x['seq'][x['pc']]
                if max([x['ready']] + [x['bars'][k] for k in range(6) if wait >> k & 1]) > t or free[p][pp] > t:
                    continue
                free[p][pp] = t + occ
                x['ready'], x['last'] = t + stall, t
                if wr != 7:
                    x['bars'][wr] = t + lat
                if rd != 7:
                    x['bars'][rd] = t + 8
                x['pc'] += 1
                if bar:
                    x['at_bar'] = True
                    waiting[x['b']] += 1
                    if waiting[x['b']] == 4:
                        waiting[x['b']] = 0
                        for y in W:
                            if y['b'] == x['b']:
                                y['at_bar'], y['ready'] = False, max(y['ready'], t + 2)
                if x['pc'] == len(x['seq']):
                    x['r'] += 1
                    if x['r'] < rounds:
                        load(x)
                break
        t += 1
        if t0 is None and all(y['r'] >= 2 for y in W):
            t0 = t
    return (t - t0) / (rounds - 2)


def main():
    args = sys.argv[1:]
    opt = lambda name, dflt=None: args[args.index(name) + 1] if name in args else dflt
    nvrtc, ptxas, nvdisasm = opt('--nvrtc'), opt('--ptxas'), opt('--nvdisasm')
    if not (nvrtc and ptxas and nvdisasm):
        raise SystemExit(__doc__)
    funcs, labels = parse(compile_module(nvrtc, ptxas, nvdisasm, opt('--module', 'kang4g'), opt('--arch', 'sm_86')))
    rounds = int(opt('--rounds', 6))
    other, inv = round_path(funcs, labels, False), round_path(funcs, labels, True)
    finv, fparts = inv_path(funcs, labels)
    for nm, s in fparts:
        n, a, f, o, st = stats(s)
        print('fe_inv, %-24s %5d instructions (ALU %d, FMA %d, other %d), stall sum %d' % (nm, n, a, f, o, st))
    n, a, f, o, st = stats(finv)
    print('fe_inv executed by one warp: %d instructions, stall sum %d' % (n, st))
    for nm, s in (('a work-item, per round', other), ('the inverting warp adds', inv)):
        n, a, f, o, st = stats(s)
        if s is inv:
            n0, a0, f0, o0, st0 = stats(other)
            n, a, f, o, st = n - n0, a - a0, f - f0, o - o0, st - st0
        print('%-24s %5d instructions (ALU %d, FMA %d, other %d), stall sum %d, and fe_inv' % (nm, n, a, f, o, st))
    a, mo, c = split(other)
    _, mid, _ = split(inv)
    m = []
    for x in mid:
        m.append(x)
        if 'CALL.REL.NOINC' in x[1] and 'fe_inv' in x[1]:
            m += finv
    print('stall sums of a round: %d before the first barrier, %d for the inverting warp between the barriers, %d after the second'
          % (stats(a)[4], stats(m)[4], stats(c)[4]))
    steps = 4 * 128
    for grps, what in (((0,), 'one block'), ((0, 21), 'two blocks, inverting on different partitions'), ((0, 20), 'two blocks, inverting on the same partition')):
        for wide in (1, 2):
            cyc = simulate((a, m, mo, c), grps, rounds, wide)
            k = steps * len(grps)
            print('%-48s IMAD.WIDE %d slot%s: %6.0f cycles per round, %.4f steps per cycle and SM: %4.0f M steps/s at 1.3 GHz, %4.0f M at 1.89 GHz on 20 SMs'
                  % (what, wide, 's' if wide > 1 else ' ', cyc, k / cyc, k / cyc * 20 * 1.3e3, k / cyc * 20 * 1.89e3))


if __name__ == '__main__':
    main()
