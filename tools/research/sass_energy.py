#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Energy model of the CUDA linear kernel per key, from its SASS, for a card that its power limit holds below its top clock.

    tools/research/sass_energy.py --nvrtc LIBNVRTC --ptxas PTXAS --nvdisasm NVDISASM [--root DIR] [--module NAME] [--warps N]
                                  [--fwd N] [--dram BYTES] [--arch sm_86] [--seeds N] [--draws N] [--save FILE] [--ref FILE] [-DNAME ...]

The executed path of one iteration of the loops over the keys (the forward loop of the batch inversion, then the pair loop with the
calls of hash160_w2 expanded) is rebuilt from the SASS as tools/research/sass_sched.py does, and the issue model of that script gives
the cycles per 32 keys of an SM partition (with an IMAD.HI at 2.4 issue slots, as measured, and an IMAD.WIDE at 1, 2 or 2.4; the mean
over --seeds staggered starts of the warps, 6 by default: one start alone moves the result by up to 2 %). --root DIR compiles the
kernels of another checkout (the counts of 1.17.0). Every instruction of the path is counted by class (IMAD.WIDE, IMAD as an addition, other
IMAD, the moves, IADD3, LOP3, SHF, LEA, PRMT, SEL, ISETP, loads and stores) and by operand traffic: the register reads (a 64-bit addend
of IMAD.WIDE or an address .64 is two, the data of a 128-bit store four), those served by the operand reuse cache (the .reuse flags
of nvdisasm), the constant-bank and uniform operands, and the register writes. The energy of a key is

    E = sum over the instructions of (issue + reads + writes + datapath + L1 for a load or store) + cycles x per-cycle + DRAM bytes x per-byte

in units of one warp-wide read of a 32-bit register (1024 bits). --fwd is the number of denominators one iteration of the forward
loop handles (1 unless the loop was unrolled), --dram the bytes of video-memory traffic per key (the prefix products of the batch
inversion: 32 when all of them are kept, 16 when every other one is). The weights are not known for this chip: every draw takes each
weight at random (log-uniform) within its range below, and the per-cycle term is the one that makes the draw reproduce the A2000's
measurement of 1.17.0 against 1.18.0 (793 and 882 M keys/s, both power-bound) for a dependence of the energy of an operation on the
clock of V^2 ~ f^eps, eps from 0 to 1. --save writes the counts of the module to a file; --ref FILE compares the module with saved
counts (the ratio of keys per second at the same power, over all the draws). Ranges and sources: docs/en/RESEARCH.md section 5,
after 1.19.1.

Needs the tools of sass_sched.py (NVIDIA's pip wheels and the cuda-nvdisasm-12-0 package)."""
import collections, importlib.util, json, math, os, random, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('sass_sched', os.path.join(HERE, 'sass_sched.py'))
ss = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ss)

# (low, high) of each weight, in warp-wide 32-bit register reads
RANGES = {
    'issue': (0.3, 1.0),        # fetch, decode, scoreboard and dispatch of one warp instruction
    'write': (1.0, 1.5),        # a warp-wide 32-bit register write
    'reuse': (0.1, 0.25),       # an operand from the reuse cache
    'konst': (0.05, 0.2),       # a constant-bank or uniform operand (one value for the warp)
    'alu': (0.02, 0.1),         # the datapath of an add, logic op, shift or select on 32 lanes
    'imad1': (0.05, 0.3),       # an IMAD whose multiplier is 1: one partial product row toggles
    'imul': (0.3, 1.0),         # a 32x32-bit IMAD with variable operands
    'wide': (0.5, 1.6),         # IMAD.WIDE: the full product, two results
    'l1': (2.0, 8.0),           # one load or store instruction through L1 (tags, data array, crossbar)
    'dram': (0.7, 3.0),         # one byte of GDDR6 traffic (5 to 8 pJ/bit against 20 to 60 pJ for a 1024-bit register read)
}
NOWRITE = ('ST', 'BRA', 'CALL', 'RET', 'BSSY', 'BSYNC', 'EXIT', 'RED', 'BAR', 'WARPSYNC', 'NOP')


def build_trace(funcs, labels):
    """the executed instructions of one turn of the forward loop and of the pair loop, with hash160_w2 expanded, and the length of the
    first: sass_sched.build_trace, but a loop may hold the CALL by which ptxas leaves an unrolled loop (no function is called), and a turn
    of the pair loop may hold more than two keys"""
    main = funcs['pz_search']
    ins = [t for _, t, _ in main]
    lab = {k: v[1] for k, v in labels.items() if v[0] == 'pz_search'}
    hname = [f for f in funcs if 'hash160_w2' in f][0]
    fcall = lambda t: 'CALL' in t and '$' in t
    loops = []
    for i, t in enumerate(ins):
        m = re.search(r'BRA\s+`\(\.(L_x_\d+)\)', t)
        if m and m.group(1) in lab and lab[m.group(1)] <= i:
            loops.append((lab[m.group(1)], i))
    s, e = min([(a, b) for a, b in loops if any('hash160_w2' in x for x in ins[a:b + 1])], key=lambda x: x[1] - x[0])
    trace, i = [], s
    rare = re.compile(r'@!?P\d BRA `\(\.(L_x_\d+)\)')
    while i <= e:
        if fcall(ins[i]) and 'hash160_w2' in ins[i]:
            trace.extend(funcs[hname])                 # the callee, its RET included
            j = i + 1
            while not rare.match(ins[j]):
                trace.append(main[j])
                j += 1
            trace.append(main[j])                      # the branch over the rare block (the full hash of a key that passes the filter)
            i = lab[rare.match(ins[j]).group(1)]
            continue
        trace.append(main[i])
        i += 1
    inv_call = [i for i, t in enumerate(ins) if fcall(t) and 'fe_inv' in t][0]
    fa, fb = max([(a, b) for a, b in loops if b < inv_call and not any(fcall(y) for y in ins[a:b + 1])], key=lambda x: x[1] - x[0])
    return [main[k] for k in range(fa, fb + 1)] + trace, fb - fa + 1


HI_SLOTS = 2.4               # IMAD.HI: mad.hi runs at 1.00 T/s against 2.42 T/s for mad.lo in the probes of bench on the A2000


def run(trace, W, mem, l1, wide, iters=12, seed=1):
    """sass_sched.run with an IMAD.HI at HI_SLOTS issue slots (sass_sched counts it as one)"""
    cls = []
    for _, t, (stall, _, wr, rd, wait) in trace:
        op = ss.opname(t)
        p = ss.pipe(op)
        base = op.split('.')[0]
        lat = mem if base in ('LDL', 'STL') else l1 if base in ('LDG', 'LDC', 'LD', 'ULDC') else 20
        occ = 2 * wide if op.startswith('IMAD.WIDE') else 2 * HI_SLOTS if op.startswith('IMAD.HI') else 2 if p in 'AF' else 1
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
        for w in sorted(range(W), key=lambda w: last[w]):
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


def klass(op, ops, ones):
    b = op.split('.')[0]
    if op.startswith('IMAD.WIDE'):
        return 'IMAD.WIDE'
    if b == 'IMAD':
        if op.startswith('IMAD.MOV') or (op.startswith('IMAD.U32') and ops[1:3] == ['RZ', 'RZ']):
            return 'move'
        if op.startswith('IMAD.IADD') or any(o in ('c[0x3][0x0]', 'c[0x3][0x4]') or o.split('.')[0] in ones for o in ops[1:]):
            return 'IMAD add'
        return 'IMAD mul'
    if b == 'MOV':
        return 'move'
    if b in ('IADD3', 'LOP3', 'SHF', 'LEA', 'PRMT', 'SEL', 'ISETP'):
        return b
    if b in ('LDG', 'LDL', 'STL', 'STG', 'LDS', 'STS', 'LD', 'ST', 'LDC', 'ATOM', 'ATOMG', 'RED'):
        return 'load/store'
    if b in ss.ALU:
        return 'ALU other'
    return 'other'


def operands(text):
    """opcode and the list of operands of a SASS line (predicate guard removed)"""
    t = re.sub(r'^@!?U?P\w+\s+', '', text)
    op, _, rest = t.partition(' ')
    return op, [o.strip() for o in rest.split(',')] if rest.strip() else []


def traffic(op, ops):
    """register reads, reads from the reuse cache, constant/uniform operands, register writes"""
    rd = rc = k = wr = 0
    b = op.split('.')[0]
    width = 4 if '.128' in op else 2 if '.64' in op else 1
    srcs = ops
    if ops and not b.startswith(NOWRITE) and not op.startswith(NOWRITE):
        d = ops[0]
        if re.match(r'^R\d+$', d):
            wr = 2 if op.startswith('IMAD.WIDE') else width if b.startswith('LD') else 1
        elif re.match(r'^UR\d+$', d):
            k += 1
        srcs = ops[1:]
    nreg = 0
    for o in srcs:
        o = o.lstrip('-~!|')
        if o.startswith('['):
            for m in re.finditer(r'(U?R)(\d+)(\.64)?', o):
                if m.group(1) == 'R':
                    rd += 2 if m.group(3) else 1
                else:
                    k += 1
            continue
        m = re.match(r'^(U?R)(\d+|Z)(\.reuse)?', o)
        if m and m.group(2) != 'Z':
            if m.group(1) == 'UR':
                k += 1
                continue
            nreg += 1
            n = 2 if op.startswith('IMAD.WIDE') and nreg == 3 else width if b.startswith('ST') else 1
            if m.group(3):
                rc += n
            else:
                rd += n
        elif o.startswith('c['):
            k += 1
    return rd, rc, k, wr


def count(trace):
    """per class: instructions, reads, reuse reads, constant operands, writes"""
    ones, cnt = set(), collections.defaultdict(lambda: [0, 0, 0, 0, 0])
    for _, t, _ in trace:
        op, ops = operands(t)
        c = klass(op, ops, ones)
        if ops and re.match(r'^R\d+$', ops[0]):
            if op.startswith('IMAD.MOV') and ops[-1] == 'c[0x3][0x0]':
                ones.add(ops[0])
            else:
                ones.discard(ops[0])
        r = traffic(op, ops)
        e = cnt[c]
        e[0] += 1
        for i in range(4):
            e[i + 1] += r[i]
    return dict(cnt)


DP = {'IMAD.WIDE': 'wide', 'IMAD add': 'imad1', 'IMAD mul': 'imul', 'move': 'alu', 'IADD3': 'alu', 'LOP3': 'alu', 'SHF': 'alu', 'LEA': 'alu',
      'PRMT': 'alu', 'SEL': 'alu', 'ISETP': 'alu', 'ALU other': 'alu'}


def energy(cnt, w, scale=1.0):
    """energy of the instructions of `cnt` (scaled), without the per-cycle term and the DRAM"""
    e = 0.0
    for c, (n, rd, rc, k, wr) in cnt.items():
        e += scale * (n * w['issue'] + rd + rc * w['reuse'] + k * w['konst'] + wr * w['write'] + (n * w[DP[c]] if c in DP else 0)
                      + (n * w['l1'] if c == 'load/store' else 0))
    return e


def draw(rng):
    return {k: math.exp(rng.uniform(math.log(a), math.log(b))) for k, (a, b) in RANGES.items()}


R1718 = 882.0 / 793.0        # the A2000, bench, 1.17.0 -> 1.18.0
WIDE = (1, 2, 2.4)           # issue slots of an IMAD.WIDE: not known (an IMAD.HI takes 2.4 by the probes of bench on the A2000)


def main():
    args = sys.argv[1:]
    opt = lambda name, dflt=None: args[args.index(name) + 1] if name in args else dflt
    nvrtc, ptxas, nvdisasm = opt('--nvrtc'), opt('--ptxas'), opt('--nvdisasm')
    module, arch = opt('--module', 'linear512m4'), opt('--arch', 'sm_86')
    warps, fwd, dram, draws = int(opt('--warps', 4)), int(opt('--fwd', 1)), float(opt('--dram', 32)), int(opt('--draws', 2000))
    seeds = int(opt('--seeds', 6))
    if opt('--root'):
        ss.ROOT = os.path.abspath(opt('--root'))
    extra = [a for a in args if a.startswith('-D')]
    if not (nvrtc and ptxas and nvdisasm):
        raise SystemExit(__doc__)
    funcs, labels = ss.parse(ss.compile_module(nvrtc, ptxas, nvdisasm, module, arch, extra))
    trace, nfwd = build_trace(funcs, labels)
    keys = sum(1 for _, t, _ in trace[nfwd:] if 'RET' in t.split()[0])     # one expanded call of hash160_w2 per key
    # the forward loop runs once for every `fwd` denominators, and a denominator serves two keys
    fscale = keys / 2.0 / fwd
    cf, cp = count(trace[:nfwd]), count(trace[nfwd:])
    per = collections.defaultdict(lambda: [0.0] * 5)
    for cnt, s in ((cf, fscale / keys), (cp, 1.0 / keys)):
        for c, v in cnt.items():
            for i in range(5):
                per[c][i] += v[i] * s
    # cycles: the issue model on a trace of exactly the work of `keys` keys
    full = trace[:nfwd] * int(round(fscale)) + trace[nfwd:] if abs(fscale - round(fscale)) < 1e-9 else None
    if full is None:
        raise SystemExit('the forward loop does not divide the pair loop (--fwd)')
    cyc, idle, spread = {}, {}, {}
    for wd in WIDE:                     # the mean over `seeds` staggered starts of the warps; run() assumes two keys per trace
        c2 = [run(full, warps, 500, 35, wd, seed=sd) for sd in range(1, seeds + 1)]
        cyc[str(wd)] = sum(c[0] for c in c2) / seeds * 2.0 / keys
        idle[str(wd)] = sum(100.0 * sum(c[1].values()) / c[2] for c in c2) / seeds
        spread[str(wd)] = (max(c[0] for c in c2) - min(c[0] for c in c2)) * 2.0 / keys
    print('%s (%s, %d warps per partition): %d keys per iteration of the pair loop' % (module, arch, warps, keys))
    print('  %-11s %8s %8s %8s %8s %8s' % ('per key', 'instr', 'reads', 'reuse', 'const', 'writes'))
    tot = [0.0] * 5
    for c in sorted(per, key=lambda c: -per[c][0]):
        print('  %-11s %8.1f %8.1f %8.1f %8.1f %8.1f' % ((c,) + tuple(per[c])))
        tot = [a + b for a, b in zip(tot, per[c])]
    print('  %-11s %8.1f %8.1f %8.1f %8.1f %8.1f' % (('total',) + tuple(tot)))
    print('  cycles per 32 keys per partition, IMAD.WIDE at 1, 2 and 2.4 issue slots: %s; DRAM %.0f bytes per key'
          % (', '.join('%.0f (idle %.1f%%, spread %.0f)' % (cyc[str(wd)], idle[str(wd)], spread[str(wd)]) for wd in WIDE), dram))
    me = {'module': module, 'cnt': {c: list(v) for c, v in per.items()}, 'cyc': cyc, 'dram': dram}
    if opt('--save'):
        json.dump(me, open(opt('--save'), 'w'))
    if not opt('--ref'):
        return 0
    # with the reference (what is compared) and the two measured forms of 1.17.0 and 1.18.0 (saved with --save as v117 and v118)
    ref = json.load(open(opt('--ref')))
    v17, v18 = [json.load(open(os.path.join(os.path.dirname(os.path.abspath(opt('--ref'))), n + '.json'))) for n in ('v117', 'v118')]
    rng = random.Random(7)
    q = lambda a, p: a[min(len(a) - 1, int(p * len(a)))]
    rg = lambda a: '%+.1f%% (%+.1f .. %+.1f)' % (100 * (q(a, 0.5) - 1), 100 * (a[0] - 1), 100 * (a[-1] - 1))
    pc = lambda a: '%.0f%% (%.0f .. %.0f)' % (100 * q(a, 0.5), 100 * a[0], 100 * a[-1])
    for wd in WIDE:
        k = str(wd)
        for eps in (0.0, 0.5, 1.0):
            out, einst, cshare, dshare, fail = [], [], [], [], 0
            for _ in range(draws):
                w = draw(rng)
                E = lambda m: energy(m['cnt'], w) + m['dram'] * 32 * w['dram']          # per 32 keys (one warp-key)
                # the per-cycle energy that reproduces 793 -> 882: (E17 + cy C17) C17^eps = R (E18 + cy C18) C18^eps, R = 1.112^(1+eps)
                R = R1718 ** (1 + eps)
                c17, c18 = v17['cyc'][k], v18['cyc'][k]
                cy = (R * E(v18) * c18 ** eps - E(v17) * c17 ** eps) / (c17 ** (1 + eps) - R * c18 ** (1 + eps))
                if cy < 0:
                    fail += 1
                    continue
                tot = lambda m: (E(m) + cy * m['cyc'][k]) * m['cyc'][k] ** eps
                out.append((tot(ref) / tot(me)) ** (1.0 / (1 + eps)))
                einst.append(E(me) / E(ref))
                cshare.append(cy * me['cyc'][k] / (E(me) + cy * me['cyc'][k]))
                dshare.append(me['dram'] * 32 * w['dram'] / (E(me) + cy * me['cyc'][k]))
            for a in (out, einst, cshare, dshare):
                a.sort()
            if not out:
                print('  wide %s, eps %.1f: no draw reproduces 1.17.0 -> 1.18.0' % (k, eps))
                continue
            print('  wide %s, eps %.1f: keys/s at the same power against %s %s; energy of the instructions and DRAM %s; in the energy of %s:'
                  ' per-cycle term %s, DRAM %s; %d of %d draws cannot reproduce 1.17.0 -> 1.18.0'
                  % (k, eps, ref['module'], rg(out), rg(einst), module, pc(cshare), pc(dshare), fail, draws))
    return 0


if __name__ == '__main__':
    sys.exit(main())
