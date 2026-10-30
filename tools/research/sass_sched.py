#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Issue model of one SM partition running the CUDA linear kernel, from the scheduling control words of its SASS.

    tools/research/sass_sched.py --nvrtc LIBNVRTC --ptxas PTXAS --nvdisasm NVDISASM [--module NAME] [--warps N] [--mem CYCLES]
                                 [--l1 CYCLES] [--wide SLOTS] [--arch sm_86] [-DNAME ...]

The module (default linear512m4, from src/kernels/ptx/modules.txt, with any extra -D options) is compiled with NVRTC and ptxas as
tools/gen_ptx.py does, disassembled with the instruction encodings (nvdisasm -hex), and the control words that ptxas writes into every
instruction are read: the stall count (the cycles before the same warp may issue again, which encodes the fixed latencies of the
dependencies and the two cycles a 16-lane pipe holds a warp instruction), the yield bit, and the scoreboard barriers of the variable-latency
operations (loads, stores, calls). The per-pair trace of the kernel is rebuilt from the executed path (the forward loop, then the pair loop
with the two calls of hash160_w2 expanded and the rare blocks skipped) and N warps run it on one partition: one instruction issued per cycle,
IADD3/LOP3/SHF/LEA/SEL/PRMT/ISETP on the ALU pipe and IMAD on the FMA pipe, each holding its pipe two cycles (IMAD.WIDE 2 x --wide), the
local-memory operations at --mem cycles (the private arrays of a work-item are in video memory on the reference card) and the table and
constant loads at --l1 cycles. It prints the instructions per pipe, the sum of the stall counts (the time of one warp alone), the bound
T = max(2A, 2F, A + F + O) of RESEARCH.md section 5 and the modelled cycles per 32 keys per partition, with the share of idle issue cycles
and their cause (no warp ready: latency; a warp ready but its pipe busy: the pipes are unevenly loaded while the warps run the same phase).

Used for docs/en/RESEARCH.md section 5 (after 1.18.0). The tools come from NVIDIA's pip wheels and the cuda-nvdisasm-12-0 package, see
sass_count.py."""
import collections, ctypes, importlib.util, os, random, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ALU = {'IADD3', 'LOP3', 'SHF', 'LEA', 'SEL', 'PRMT', 'ISETP', 'FLO', 'POPC', 'BREV', 'MOV', 'IMNMX', 'PLOP3', 'P2R', 'R2P', 'BMSK', 'VOTE', 'IABS'}


def control(hi):
    """the control fields of the high 64 bits of an instruction: stall count, yield, write and read barrier, wait mask"""
    c = hi >> 41
    return c & 0xf, (c >> 4) & 1, (c >> 5) & 7, (c >> 8) & 7, (c >> 11) & 0x3f


def opname(t):
    return re.sub(r'^@!?U?P\w+\s+', '', t).split()[0]


def pipe(op):
    b = op.split('.')[0]
    return 'F' if b == 'IMAD' else 'A' if b in ALU else 'O'


def compile_module(nvrtc, ptxas, nvdisasm, module, arch, extra):
    spec = importlib.util.spec_from_file_location('gen_ptx', os.path.join(ROOT, 'tools', 'gen_ptx.py'))
    gp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gp)
    mods = {m[0]: m for m in gp.read_modules()}
    if module not in mods:
        raise SystemExit('unknown module %s (see src/kernels/ptx/modules.txt)' % module)
    name, files, defines = mods[module]
    ptx = gp.compile_one(ctypes.CDLL(nvrtc), name, files, defines + extra)
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
    """{function: [(offset, text, control)]} and {label: (function, index)} from nvdisasm -hex -c"""
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
            funcs[cur].append(pend + (control(int(m.group(1), 16)),))
            pend = None
    return funcs, labels


def build_trace(funcs, labels):
    """the executed instructions of one pair of keys: the forward loop body, then the pair loop with hash160_w2 expanded"""
    main = funcs['pz_search']
    ins = [t for _, t, _ in main]
    lab = {k: v[1] for k, v in labels.items() if v[0] == 'pz_search'}
    hname = [f for f in funcs if 'hash160_w2' in f][0]
    loops = []
    for i, t in enumerate(ins):
        m = re.search(r'BRA\s+`\(\.(L_x_\d+)\)', t)
        if m and m.group(1) in lab and lab[m.group(1)] <= i:
            loops.append((lab[m.group(1)], i))
    s, e = min([(a, b) for a, b in loops if any('hash160_w2' in x for x in ins[a:b + 1])], key=lambda x: x[1] - x[0])
    trace, i = [], s
    while i <= e:
        if 'CALL' in ins[i] and 'hash160_w2' in ins[i]:
            trace.extend(funcs[hname])                 # the callee, its RET included
            j = i + 1
            while not re.match(r'@!?P\d BRA `\(\.(L_x_\d+)\)', ins[j]):
                trace.append(main[j])
                j += 1
            trace.append(main[j])                      # the branch over the rare block (the full hash of a key that passes the filter)
            i = lab[re.match(r'@!?P\d BRA `\(\.(L_x_\d+)\)', ins[j]).group(1)]
            continue
        trace.append(main[i])
        i += 1
    inv_call = [i for i, t in enumerate(ins) if 'CALL' in t and 'fe_inv' in t][0]
    fa, fb = max([(a, b) for a, b in loops if b < inv_call and not any('CALL' in y for y in ins[a:b + 1])], key=lambda x: x[1] - x[0])
    return [main[k] for k in range(fa, fb + 1)] + trace, fb - fa + 1


def run(trace, W, mem, l1, wide, iters=12, seed=1):
    """cycles per 32 keys per partition for W warps on the trace, and the idle issue cycles by cause"""
    cls = []
    for _, t, (stall, _, wr, rd, wait) in trace:
        op = opname(t)
        p = pipe(op)
        base = op.split('.')[0]
        lat = mem if base in ('LDL', 'STL') else l1 if base in ('LDG', 'LDC', 'LD', 'ULDC') else 20
        occ = 2 * wide if op.startswith('IMAD.WIDE') else 2 if p in 'AF' else 1
        cls.append((p, occ, max(stall, 1), wait, wr, rd, lat))
    n = len(trace)
    random.seed(seed)
    pc, ready = [0] * W, [random.randint(0, n // W * k) for k in range(W)]      # staggered starts
    bars = [[0] * 6 for _ in range(W)]
    pipe_free = {'A': 0, 'F': 0, 'O': 0}
    done, last = [0] * W, [-1] * W
    t, total = 0, iters * n
    idle = collections.Counter()
    while min(done) < total:
        cand, any_ready = None, False
        for w in sorted(range(W), key=lambda w: last[w]):                       # loose round robin: the warp that waited longest
            if done[w] >= total:
                continue
            p, occ, stall, wait, wr, rd, lat = cls[pc[w]]
            r = max([ready[w]] + [bars[w][b] for b in range(6) if wait >> b & 1])
            if r <= t:
                any_ready = True
                if pipe_free[p] <= t:
                    cand = w
                    break
        if cand is None:
            idle['pipe busy' if any_ready else 'no warp ready'] += 1
            t += 1
            continue
        w = cand
        p, occ, stall, wait, wr, rd, lat = cls[pc[w]]
        pipe_free[p] = t + occ
        ready[w] = t + stall
        if wr != 7:
            bars[w][wr] = t + lat
        if rd != 7:
            bars[w][rd] = t + 8
        last[w] = t
        pc[w] = (pc[w] + 1) % n
        done[w] += 1
        t += 1
    return t / (iters * W) / 2.0, idle, t


def main():
    args = sys.argv[1:]
    opt = lambda name, dflt=None: args[args.index(name) + 1] if name in args else dflt
    nvrtc, ptxas, nvdisasm = opt('--nvrtc'), opt('--ptxas'), opt('--nvdisasm')
    module, arch = opt('--module', 'linear512m4'), opt('--arch', 'sm_86')
    warps, mem, l1, wide = opt('--warps'), int(opt('--mem', 500)), int(opt('--l1', 35)), int(opt('--wide', 1))
    extra = [a for a in args if a.startswith('-D')]
    if not (nvrtc and ptxas and nvdisasm):
        raise SystemExit(__doc__)
    funcs, labels = parse(compile_module(nvrtc, ptxas, nvdisasm, module, arch, extra))
    for f, ins in funcs.items():
        pa, sa = collections.Counter(), collections.Counter()
        for _, t, (stall, _, _, _, _) in ins:
            p = pipe(opname(t))
            pa[p] += 1
            sa[p] += stall
        print('%-28s %5d instructions: ALU %4d, FMA %4d, other %3d; stall counts %5d' % (f[:28], len(ins), pa['A'], pa['F'], pa['O'], sum(sa.values())))
    trace, nfwd = build_trace(funcs, labels)
    pa = collections.Counter(pipe(opname(t)) for _, t, _ in trace)
    stalls = sum(c[0] for _, _, c in trace)
    T = max(2 * pa['A'], 2 * pa['F'], len(trace)) // 2
    print('per pair of keys: %d instructions (forward loop %d), ALU %d, FMA %d, other %d; one warp alone %d cycles; T = %d cycles per 32 keys per partition'
          % (len(trace), nfwd, pa['A'], pa['F'], pa['O'], stalls, T))
    for W in ([int(warps)] if warps else [2, 3, 4, 5, 6]):
        cyc, idle, t = run(trace, W, mem, l1, wide)
        print('%d warps per partition (mem %d, l1 %d, wide %d): %.0f cycles per 32 keys, idle %.1f%% (pipe busy %.1f%%, no warp ready %.1f%%); %.0f M keys/s at 1.89 GHz on 80 partitions'
              % (W, mem, l1, wide, cyc, 100.0 * sum(idle.values()) / t, 100.0 * idle['pipe busy'] / t, 100.0 * idle['no warp ready'] / t, 1.89e9 * 80 * 32 / cyc / 1e6))


if __name__ == '__main__':
    main()
