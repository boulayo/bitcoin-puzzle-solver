# Research log: what was tried and what was measured

This file keeps the development history behind [`ALGORITHMS.md`](ALGORITHMS.md): the variants that were tried and discarded, the intermediate
measurements and the analyses of designs that the program no longer uses (the centered Gaudry-Schost layout, schemas 1 and 2 in detail, Karatsuba, the
herd of walkers in shared memory or in L2, the precomputation study, the counting of pairs with integer values), and the low-level detail of the GPU
and SIMD arithmetic. What the program does today is in ALGORITHMS.md. The text was moved from there without changes, apart from the file name added to
the cross-references; the figures are those of the version named in each section, and several are superseded by later sections (the corrections are
noted where they are).

**Section numbers.** The sections keep the numbers they had when everything was in one document, because the source, the tools and the other
documents cite them. Sections 4.5, 4.6, 8.6, 8.7 and 8.9 are only here; the others here give the earlier layouts and figures behind the section of the
same number in ALGORITHMS.md. A reference without a file name is to a section of the same file. "Schema" is explained in ALGORITHMS.md section 4.

## Contents

4. The interval kangaroo
   * 4.1 Setup of schema 1
   * 4.2 Why it costs less: from the centered layout to schema 1
   * 4.3 Unproductive cycles and drift: how the defences came
   * 4.4 Parameters of schema 1
   * 4.5 Measurements (real curve, `pzsolve kstat BITS TRIALS THREADS`)
   * 4.6 Things evaluated and rejected
   * 4.7 Resuming: the log formats of the older layouts
5. GPU (CUDA and OpenCL)
8. Theoretical limits: how much can still be improved?
   * 8.1 The constant: the figures of the earlier layouts
   * 8.4 Cost of a step and hardware bottlenecks (1.5.0)
   * 8.5 Comparison with RCKangaroo: what followed for this program
   * 8.6 Modular multiplication: what was tried
   * 8.7 How low can it go: counting pairs with integer values
   * 8.8 Below 1.085: species with fixed residues (schema 2)
   * 8.9 More RAM or a precalculation: what can be bought
   * 8.10 Local walks, long jumps and banded zones (schema 3)
   * 8.11 BSGS: the cells of the table and the cost of a lookup
   * 8.12 Where the constant of schema 3 goes, and what is left
   * 8.13 The last percent: the delay of the distinguished points, and what the layout could still give
   * 8.14 The constant, once more (after 1.19.2)
10. SIMD lane arithmetic (`avx2v`, `avx512v`, `avx512i`)

## 4. The interval kangaroo (public key known)

The program uses schema 3 (ALGORITHMS.md section 4). Subsections 4.1 to 4.4 and 4.7 below are the text that described the earlier
layouts in ALGORITHMS.md when they were still in the program; it was moved here without changes.

**Which schema the program uses.** The kangaroo went through three schemas during development. They share the step and the equivalence classes
and differ in the species of walkers, where they start, the length of the jumps and how a pair of distinguished points is resolved. **The program
implements schema 3** (the species and residues of schema 2, long jumps of mean `W^¾/16` and start zones split into bands; sections 8.8 and 8.10).
Schemas 1 and 2 were used by development builds (1.6.0 to 1.7.8) and, from 1.8.0 to 1.9.0, schema 1 for campaigns with fewer than 256 kangaroos;
version 1.9.1 removed them together with their log formats (a `dp.bin` written by them is kept aside and not used). This section describes schema 1, which is the simplest and the base of the later
ones: 4.1 to 4.6 (setup, parameters, measurements) refer to it, while 8.8 and 8.10 give the changes of schemas 2 and 3 and their measured cost
(≈ 1.10 and ≈ 1.06 against 1.15·√W).

### 4.1 Setup of schema 1 (from 1.6.0)

This subsection describes **schema 1**, the layout of the development builds 1.6.0 to 1.7.7. Schema 2 has the same classes and the same step but
three species of walkers with fixed residues mod 12 (section 8.8), about 4% fewer steps; schema 3, the one the program uses, is in section 8.10.

Let `S = W/32` (or less, if the interval starts below `W/32`), `base = lo − S` and `Q' = Q − base·G`. The key is
`k = base + x` with `x ∈ [S, S + W)`: the whole interval lies on one side of the point `0` around which the classes fold.

**Equivalence classes.** `P` and `−P` have the same x-coordinate; we work on the classes `{P, −P}` and choose as
representative the point with even `y`. The walker (kangaroo) carries with it:

* the canonical point `P = e·G` and its *exact* exponent `e` as a signed 192-bit integer:
  * **tame** kangaroo (one third): starts at `e = t`, `t ∈ [1, W/32)`, and keeps `e = d`;
  * **wild** kangaroo (two thirds): starts at `e = −x + v`, `v` even in `[0, W + W/16)` (that is, from the point `v·G − Q'`), and keeps
    `e = σ·x + d`, with `σ = ±1` (it changes sign when canonicalization flips the point).

Step: `j = x(P) mod 256`, `P ← canon(P + S_j)`, `d ← d + s_j`; if `y` is odd the point is negated (`d ← −d`, `σ ← −σ`).
The jumps `s_j` (256, **even**, uniform in `[2, 24·√W]`, mean `12·√W`) are pseudo-random from the seed. Since the representative
has a random sign, the exponent does a symmetric walk (no drift): each walker explores the surroundings of its
starting point.

**Distinguished points (DP).** When `x.n[1]` has `dp_bits` low zero bits, `(x-coordinate, type, d)` is recorded in a table; two
records with the **same class but different type** reveal `x` (type ∈ {tame, wild σ=+1, wild σ=−1}):

| pair                           | equation from the same class | solution             |
|--------------------------------|------------------------------|----------------------|
| tame `d_t`, wild(+1) `d_w`     | `d_t = x + d_w`              | `x = d_t − d_w`      |
| tame `d_t`, wild(−1) `d_w`     | `d_t = −x + d_w`             | `x = d_w − d_t`      |
| wild(+1) `d₁`, wild(−1) `d₂`   | `x + d₁ = −x + d₂`           | `x = (d₂ − d₁)/2`    |

With even jumps and even starts, `d₂ − d₁` is always even: every wild(+1)–wild(−1) meeting gives the solution.
Pairs of the same type give no information (same trajectory) and the kangaroo is restarted at a random point.
Every candidate solution is verified (`(base+x)·G` against address/pubkey) before it is accepted: false positives
of the table (62-bit tag) cannot produce wrong results.

### 4.2 Why it costs less: from the centered layout to schema 1

In the classic kangaroo (tame/wild with positive jumps only) the wild must "catch up" with the tame trail:
`≈ 2·√W` steps. With classes the points live in a roughly halved space and collisions are a *birthday problem*.

* **Up to 1.5.0** the "centered" Gaudry–Schost was used: `base` at the center of the interval, tame kangaroos uniform over the whole
  interval, wild ones in a window `±W/8` around `x`; the useful collisions were almost only tame–wild (bipartite).
  Measured: **≈ 1.26·√W** (Galbraith–Ruprai theory ≈ 1.36·√W).
* **From 1.6.0** the layout is that of the "SOTA" method of RCKangaroo (RetiredCoder, 2024; studied, its code not used):
  the wild kangaroos start from `−x + v` with `v` spread over the whole interval, so their exponents lie on **both sides**
  of zero and the classes fold onto themselves: two wild kangaroos at symmetric positions (`−x + v₁ = −(−x + v₂)`, that is
  `v₁ + v₂ = 2x`) collide and give `x`. Each wild kangaroo can meet almost all the other wild ones (birthday problem that is *not* bipartite,
  more efficient), while the tame ones cover the zone near zero, where the folded wild ones are sparser. Measured: **≈ 1.1–1.2·√W**
  (RESEARCH.md section 4.5), about **10% fewer steps**, consistent with the K = 1.15 declared by RCKangaroo.

### 4.3 Unproductive cycles and drift: how the defences came

Walking on the classes `{P, −P}`, a walker can return to a point it already visited without ever meeting a DP: from then
on it repeats the same points forever (an *unproductive* cycle). With `R` jumps in the table (Bos–Kleinjung–Lenstra, *On the use
of the negation map in the Pollard rho method*, 2010):

* **Cycles of length 2** (`P → P' → P`): probability `1/(2R)` per step. When the current point coincides with the one from
  two steps earlier, the next jump `(j+1) mod R` is used: the rule depends only on the history `(P_{n−1}, P_n)`, so two
  walkers that merge keep merging.
* **Cycles of length 4, 6, …**: even after removing those of length 2 they remain, with probability `≈ (R−1)/(4R³) ≈ 1/(4R²)` per step,
  that is **one every ≈ 2.6·10⁵ steps** with `R = 256` (measured: 3·10⁵ in the model of `tools/research/ksim.c`, almost all of length 4).
  **Up to 1.5.0 the only defence was a restart after `24·2^dp_bits` steps without a DP**: fine for small intervals, but
  with `dp_bits = 45` (puzzle 140) that threshold is never reached, and every walker stayed trapped after a few
  minutes (GPU: ~1700 steps/s per walker). The displayed speed stayed high, but from then on the search could no longer
  find collisions. In the model, with few jumps to make cycles frequent and the threshold disabled, 38–95% of the
  searches do not terminate.
* **Deterministic detection and exit (from 1.5.1)**: every `64` steps the walker stores the label of the current point
  (*checkpoint*, 64 bits of `x`) and from then on keeps the minimum label seen. If it returns to the checkpoint it is in a cycle, and
  the minimum is that of the whole cycle; it keeps going until it reaches the point with the minimum label and from there makes an **escape
  jump**, taken from a separate table of 16 jumps (same distribution as the normal ones). The exit point depends only on the
  cycle, not on where or when the walker entered it: two walkers that had merged leave in the same way and
  stay merged, so the collision is seen at the next DP (with a restart, or an exit from the detection point, the
  trails would separate at every cycle, thousands of times between one DP and the next). The escape jump is not part of the
  normal table, so the walk cannot fall into the same cycle again with a combination of normal jumps. If the cycle does not
  close within one period (length ≥ 32: negligible probability) detection starts over; the restart after
  `24·2^dp_bits` steps remains as a last defence.
  Cost: a few comparisons per step and on average ≈ 40 wasted steps per cycle (0.015%). Checks: in the model, with 16 jumps
  and the threshold disabled, every search terminates (cost 2.0·√W, against the 38% of searches stuck without the exit); on the real
  curve the self-test solves an interval of 2^30 keys with only 8 jumps (one cycle every ~250 steps), a DP every 128 steps and
  no restarts, which without the cycle exit does not terminate.
* **64-bit counter of steps without a DP** from 1.5.0: up to 1.4.0 it was 32-bit and the threshold stopped at 2^32, so with
  `dp_bits ≥ 33` almost every walker would have been restarted before reaching its distinguished point.
* **No drift to tune**: the walk on the classes is symmetric; the mean jump length (`12·√W`) is
  that of RCKangaroo, and in the simulator the outcome is almost indifferent between `0.5·√W` and `40·√W` (1.16–1.21).

### 4.4 Parameters of schema 1

Values for schema 1; schemas 2 and 3 change the walker zones and the jump table (sections 8.8 and 8.10). Here `K` is the total number of walkers.

| Parameter | Value | Reason |
|-----------|-------|--------|
| base | start − `W/32` | the interval on one side of zero, far from the folding |
| tame zone | `[1, W/32)`, one third of the walkers | as in RCKangaroo; in the simulator 1/3 is better than 1/5 and 1/2 |
| wild zone | `−x + v`, `v` even in `[0, W + W/16)` | the wild kangaroos cover the whole interval on both sides of zero |
| jumps | 256 even, uniform in `[2, 24·√W]` (+16 for cycle escape) | symmetric walk, even jumps for wild–wild collisions |
| walkers per CPU thread | `clamp(√W/(256·threads), 16, 4096)`, multiple of 16 | the batch inversion is shared by the whole herd of the thread; 4096 is the measured optimum (RESEARCH.md sec. 8.4) |
| GPU walkers | `NK` per work-item (64/128/256, tuned) × 512 work-items per SM (CUDA) | at most `√W/256` in total, so each one walks long enough |
| `dp_bits` | `max(⌊log2(0.01·√W/K)⌋, bits required by RAM, 2)` | more RAM, less overhead |
| DP table | 32 bytes/entry, grows up to the `--mem` limit (default 40% of free RAM, max 64 GiB); if the limit is reached the density of stored points is halved (only DPs with one more zero bit are kept) instead of stopping | one mutex for the table and another for the log file: DPs are rare |

**The memory margin of `dp_bits` (up to 1.11.0).** When memory decided `dp_bits`, the table had to hold the distinguished points of `4·√W`
steps. Since the table thins itself in place when it is full (ALGORITHMS.md section 4.4) the margin is 1.3 (`KANG_DP_MEM_MARGIN`): the
margin of 4 cost one or two bits wherever memory decides, on puzzles 135 and 140 with 2^20 to 2^22 walkers (GPUs) and 8 to 64 GiB of RAM up
to 29 % more steps (8 % at 32 GiB); the file of distinguished points is now 2 to 4 times larger.

### 4.5 Measurements (real curve, `pzsolve kstat BITS TRIALS THREADS`)

Schema 1. Mean cost in units of `√W` (4 threads, key uniform in the interval, automatic `dp-bits`):

| W | runs | mean 1.6.0 | median | 90th percentile | mean up to 1.5.0 (300 runs) |
|---|------|------------|--------|-----------------|-----------------------------|
| 2^28 | 400 | **1.21**·√W | 1.13·√W | 2.13·√W | 1.26·√W |
| 2^32 | 400 | **1.20**·√W | 1.10·√W | 2.06·√W | 1.23·√W |
| 2^36 | 400 | **1.11**·√W | 1.01·√W | 2.02·√W | 1.31·√W |
| 2^40 | 400 | **1.08**·√W | 1.00·√W | 1.89·√W | 1.21·√W |
| 2^44 | 400 | **1.16**·√W | 1.12·√W | 1.99·√W | 1.31·√W |

Overall mean ≈ **1.15·√W** (standard error per row ≈ 0.03, overall ≈ 0.015), against 1.26 for the
previous layout, ≈ 2·√W for the classic kangaroo and ≈ 1.36·√W for the Galbraith–Ruprai theoretical estimate.

The distribution has a long tail (it is of the "birthday" type: about `1 − exp(−(n/c)²)` with `c ≈ 1.30·√W`): median ≈ 1.08·√W,
90th percentile ≈ 2.0·√W. The program shows the estimated probability of success and the times for 50% and 90%.

### 4.6 Things evaluated and rejected

* **GLV endomorphism** (`λ·P`, classes of 6): it reduces the kangaroo/rho on the whole group, but inside an interval `λ·e mod n` leaves
  the small interval, so it creates no useful collisions.
* **BSGS with a table in RAM**: with `T` entries in the table (the x-coordinates of `t·G`, `t < T`, also hold for `−t`) every giant
  step covers `2T+1` values: mean cost `T + W/(4T)`, minimum `1.0·√W` for `T = √W/2` (worst case `1.41·√W`, or `1.5·√W` with this `T`, see ALGORITHMS.md 9.1). **With enough RAM
  it therefore beats the kangaroo (1.15·√W)**. It has been implemented since 1.1.0 (`--mode bsgs`, ALGORITHMS.md section 9); the table
  takes `≈ 2.9·√W` bytes (11 GiB at `W = 2^64`, 46 GiB at `2^68`; `≈ 6.2·√W` up to 1.10.0, section 8.11), so it is usable only for
  medium-small intervals. With the kangaroo
  constant of 1.26 used up to 1.5.0, a table smaller than `√W/2` stayed below the kangaroo for `T ≥ 0.24·√W`, that is up to `2^70`.
* **4-animal kangaroo** (≈ 1.715·√W) and Gaudry–Schost without symmetry (≈ 1.66·√W): superseded by the symmetric variant.
* **Parameters of the centered variant (up to 1.5.0, `tools/research/ksim.c`, 8000 runs per point, W = 2^28, error ≈ ±0.007)**: a grid of 45
  combinations of wild zone `B ∈ {1/16…1/4}·W`, tame fraction `{0.4, 0.5, 0.6}` and mean jump `{0.3, 0.5, 0.8}`, plus 4 widths of the
  tame zone. The point in use (`A = W/2`, `B = W/8`, half tame, jump 0.5) is the best (1.292·√W in the model); the optimum is flat and
  far-off choices cost up to 7% (tame on `0.75·W`: +17%). No room *in that family*: the gain came from changing the layout (section 4.2). In the model, 3000–4000 runs per point (error ≈ ±0.01), same number of
  walkers and `dp`: centered 1.27–1.36·√W, new layout 1.14–1.23·√W on `W = 2^26 … 2^34` and 64–2048 walkers
  (`ksim rck=1 tf=0.3333 jabs=12`).
* **Third species of walkers on `2x`** (wild with exponent `±2x + d`, starting from `2Q`, each collision with different coefficients gives
  `(c₁ − c₂)·x = d₂ − d₁`): 27 combinations of shares and widths, the best costs 1.365·√W, that is 6% **more**; more walkers
  on `2x` make it worse (up to 1.78). Their points often fall outside the tame zone and the extra pairs do not compensate.
* **Distribution of the solved keys**: the relative position in the interval of the 82 known keys is compatible with uniform
  (Kolmogorov–Smirnov D = 0.074, 5% threshold 0.15; mean 0.51; one-bits 49.3%): no part of the interval is more likely.

### 4.7 Resuming: the log formats of the older layouts

Every new DP is appended to `dp.bin` (32 bytes + a header with a fingerprint of the interval and the key). On resume the table is
rebuilt and the walkers restart from new points (a different random stream for each session); the old trails
remain useful because each of their later DPs is already in the table. A file from a different campaign is recognized and refused.
The log format is `"PZDP4"` (section 8.10). The formats `"PZDP1"` to `"PZDP3"` of the older layouts are no longer read: `hunt` keeps such a `dp.bin` as
`dp.bin.vN` and goes on with a new log, `merge` refuses it (the old versions converted them on the fly; 1.9.1 dropped this together with schemas 1 and 2).

## 5. GPU (CUDA and OpenCL)

The kernels are described in ALGORITHMS.md section 5; this is the instruction-level work on the CUDA field arithmetic and the second version of the
kangaroo kernel.

* **CUDA arithmetic (from 1.3.0).** Measured on the sm_86 (Ampere) code: per key the linear kernel executes ≈ 4200 integer
  instructions, half in hash160 (≈ 2100, already close to the optimum) and half in the field. At ≈ 513 M keys/s the reference laptop GPU (20 SM, 4 GB, 192 GB/s, 2 MiB of L2; the card on which the GPU speeds in this document were
  measured) is at the limit of its integer throughput (20 SM × 64 operations per cycle): more threads are of no use, what matters is the instruction count. For this reason
  addition, subtraction and the reduction after a product use PTX carry chains (`add.cc`/`addc`, `sub.cc`/`subc`,
  `mad.lo.cc`/`madc.hi.cc` for the multiplication by 977): add+sub from 176 to 88 instructions, multiplication from 328 to 288, a dedicated
  square (36 products instead of 64) from 328 to ≈ 232. The 8×8 product stays in C because Ampere has no native `madc.hi` (the all-PTX
  version measured 368; *in 1.14.0 the product is on PTX chains after all, in a form `ptxas` fuses: see the end of this section*). Estimate: ≈ 11% fewer instructions per key. The logic of the chains is verified with an
  exact emulation of the PTX semantics (`tests/t_gpufe.c`, 5 million combinations, edge cases of the double carries; mutation testing);
  on the device it is confirmed by the self-test.

The two bullets that described the kernels of 1.5.0 in ALGORITHMS.md section 5:

* **Linear kernel with "fused" inversion (1.5.0)**: the backward pass of Montgomery's trick derives each inverse and uses it
  immediately for the points `C ± i·G`, recomputing the denominator from the table instead of keeping it in an array. The private memory per
  work-item is halved (window 256: from 17 to 8.7 KB) and the traffic to video memory drops from ≈ 96 to ≈ 32 bytes per key
  (at ≈ 600 M keys/s it was ≈ 57 GB/s out of ≈ 190). The hash filter uses the first three words (96 bits; every candidate is re-verified
  on the CPU anyway): they do not depend on the last steps of RIPEMD-160, which the compiler eliminates (≈ 0.4% of instructions).
* **Kangaroo (rewritten in 1.5.0, kernel v2 in 1.7.0)**: up to 1.4.0 each work-item handled **16** walkers: one inversion (≈ 270 multiplications)
  every 16 steps, that is ≈ 17 multiplications per step against the ≈ 6 of the step itself, and the kernel used 255 registers with spill.
  Now `NK` = 64, 128 or 256 walkers per work-item (tuning on the device, `PZ_GPU_NK` to force it), the same "fused" scheme
  as the linear kernel (no array of denominators), **128 registers without spill** and therefore 512 resident work-items per SM. State in
  global memory (128 bytes per walker: x, y, 192-bit distance, type, sign, 32-bit anti-cycle labels, last DP);
  the DPs go out in a buffer and are inserted in the same table as the CPU's. Restarts are handled by the host.

Kangaroo kernel (rewritten in 1.5.0, kernel v2 in 1.7.0), continuing the Kangaroo bullet above:

  **Kernel v2 (1.7.0)**, built around memory traffic (sec. 8.4): (a) the distance does not travel from step to step: every
  step adds 16 bits to a *history* buffer (jump index, "negated" bit) and `d` is rebuilt by replaying the history, at the end of the
  launch for everyone and, for the rare distinguished points, on the spot; (b) one pass per round instead of two: the pass that moves the
  walkers also computes the denominators of the next round and their running products, which alternate between prefixes and suffixes (the pass
  goes down, then up, ...): Montgomery's trick works with both, and each pass reads and overwrites the products
  left by the previous one; (c) the inversion is by divstep (sec. 8.4); (d) a launch does at most 64 rounds (depth of the history; since 1.13.0 the depth is 64 to 256, chosen per herd from the device memory),
  starting from the step number common to the whole herd.

**After 1.13.0: instruction counts of the kernels.** No GPU was available: every figure is SASS from NVRTC 12.0 and `ptxas` (sm_86 unless
stated; sm_75 and sm_89 give the same within a few instructions), counted on the executed path of the loops (`nvdisasm`), or AMD ISA from
clang 18 for the OpenCL kernels. Correctness: `tests/t_gpufe.c` (5.4 million combinations per build, mutations of every new routine), the
self-test and planted-key hunts on the simulated CUDA driver and on pocl.

* **Products on `mad.wide` chains.** `ptxas` turns a `mad.lo.cc` and a `madc.hi.cc` of the same two limbs into one `IMAD.WIDE.U32` (`.X` with
  the carry) when their addends are the two halves of a 64-bit register pair. The all-PTX product of 1.3.0 (368) did not keep the pairs; the
  product written in C needs ≈ 2.9 instructions per limb product (section 8.6). Two accumulators keep every pair aligned: E takes the products
  whose position `i + j` is even, O the odd ones (each row of the schoolbook product is one chain into each), and `E + O` is formed once. The
  reduction folds `H·(2^32 + 977)` the same way: the even limbs of `H` times 977 with `L` as addend pairs, the odd ones with the limbs of `H`
  itself (the `H·2^32` term). Multiplication 235 → 121 instructions, square 160 → 81 (the routine alone; sm_61, which has no `IMAD.WIDE`:
  831 → 700 and 464 → 405). The kangaroo step (loop of `kang64`) goes from 1852 to 1185 instructions (−36 %), the linear kernel from ≈ 3371
  to ≈ 2876 per key (−14.7 %). The register-capped linear variants (`m4`) and most `b` kangaroo variants lose their spills.
* **Jump table in 16-byte loads.** The entries (80 bytes) start on 16-byte boundaries: `jx`, `jy` and the `jx` of the next denominator are 6
  loads instead of 24. The work-items of a warp read different entries, so each load instruction is served one cache line per work-item: a
  quarter of the L1 requests for the table. Step 1185 → 1167; the table of the linear kernel likewise (2876 → 2868 per key).
* **Inversion in variable time.** The 30 divsteps of a round were done one at a time, ≈ 28 instructions each; executed per warp an inversion
  cost ≈ 22,000 instructions (19 rounds for the slowest of 32 random inputs, ≈ 1160 each; the ≈ 10,500 of 8.4 was not an executed count),
  i.e. ≈ 350 per step at NK = 64, a fifth of the step after the products above. Now as libsecp256k1's `modinv32_var`: a run of zero bits of `g`
  in one go (`brev` + `bfind.shiftamt`, a sentinel bit at the steps left) and, after a swap, the bottom bits of `g` cancelled with `w·f`, `w`
  taken mod 8 where `f⁻¹ = f` (no table: indexed per work-item it would be served one address at a time from the constant cache). Simulated
  on 1280 warps of random inputs: 9.8 turns of the loop per round on average, 12.6 for the slowest of a warp (cancelling 4, 6 or 8 bits saves
  at most 0.5 turns for the cost of the inverse mod 2^k); a turn is 33 instructions. An inversion is ≈ 14,600 executed instructions per warp, of
  which ≈ 6,500 are the products of the 2×2 matrix with `d, e, f, g`: per step ≈ 120 fewer at NK = 64, 60 at NK = 128, 30 at NK = 256.
* **Byte swaps as `PRMT`.** NVRTC compiles the shift-and-mask `bswap32` of the RIPEMD-160 message to about six instructions; `__byte_perm` is
  one. Hash filter 2078 → 2030, ≈ 2867 → 2819 per key. (LLVM recognises the expression: on AMD it is already one `v_perm_b32`.)
* **OpenCL on AMD.** The portable field arithmetic was 64-bit C, two 64-bit additions per limb plus branches on the carries. With clang's
  `__builtin_addc`/`__builtin_subc` (used on AMD GPUs and on pocl only; Intel and other compilers keep a branch-free C stand-in) it is the
  same carry chains as CUDA (`v_add_co_ci_u32`), and the product goes by rows with `v_mad_u64_u32` and one chain per row. gfx1030:
  multiplication 307 → 226, square 204 → 108, addition 69 → 15, subtraction 93 → 22; loop of the kangaroo step 2529 → 1522 (gfx1100 3223 →
  1641, gfx906 2534 → 1540). The E/O form of the CUDA product does not carry over: `v_mad_u64_u32` has a carry out but no carry in.
* **Where the step stands.** Kangaroo, NK = 128: ≈ 1170 instructions plus ≈ 115 for the inversion, against ≈ 2200 before; with ≈ 215 bytes of
  traffic per step the kernel is now well on the memory side of the crossover on cards like the reference one (the `b` and `c` variants, which
  trade half or three quarters of a product for 32 or 48 bytes, have become cheaper: the tuner decides on the device). Linear: ≈ 2820 per
  key, 72 % of it the hash filter, which has no slack left (each rotation is one `SHF`, every constant word folds).
* **Tried and left out:** the tags of the last two points as a ring of two (one 4-byte store per step instead of 8): +6 instructions per step for
  the per-round slot, 2 % of the traffic, not clearly a gain; `p·md` in the update of `d, e` from the shape of `p` (three terms instead of nine
  products): −18 instructions per round of the inversion, 0.4 % of the step at NK = 64; register caps of 96 or 80 for the kangaroo (5 or 6
  blocks per SM) spill 136 and 216 bytes, 168 for the `c` variants 220; a cap of 168 for the linear kernel (3 blocks per SM) has no spill and
  the same count, but the `m4` variants (4 blocks) no longer spill either, so no third variant was added; column sums with 64-bit
  accumulators for AMD in C: 411 instructions against 307; the PTX chains emulated with the clang builtins on AMD: 1031.
* **Need a GPU:** the speed of all the above (if an `IMAD.WIDE` takes two issue cycles, as some measurements of earlier architectures suggest,
  the products gain less time than instructions: both forms have 64 of them per product); the jump table in shared memory (random 16-byte reads, ≈ 2.5-way bank conflicts against one cache line per work-item
  from L1) and the inline assembly for `v_mad_u64_u32` carries on AMD.

**In 1.15.0: normalized operands, the AMD product, and what a table could still buy.** Same method as above (SASS from NVRTC 12.0 and
`ptxas` for sm_86, AMD ISA from clang 18; sm_75 and sm_89 within a few instructions of sm_86), and the same checks (`tests/t_gpufe.c`
with mutations of the new routines, the self-test and the planted-key hunts on the simulated CUDA driver and on pocl).

* **Subtractions from normalized values.** The walker's x and y, the jump points, the centre and the table of the linear search are
  normalized (below p). Subtracting such a value borrows at most once and leaves at least 2^32 + 978, so the second correction of `fe_sub`
  cannot fire (`fe_sub_norm`, seven instructions fewer than `fe_sub`); the sum of two normalized values needs one fold (`fe_add_norm`).
  The step normalizes x3 before `x − x3` (it is normalized anyway). Kangaroo step 1167 → 1132 (`b` variants 2519 → 2443 per two steps, and
  `kang128b` loses its spill); linear 2820 → 2791 per key. The register-capped linear variants now spill 4 bytes, outside the loop over the
  keys (once per group).
* **The product on AMD in inline assembly.** `v_mad_u64_u32` adds a 32×32-bit product to a register pair and sets a carry out that C cannot
  reach, so the row-wise product needed a move to zero each high half and two additions per limb product (73 `v_mad_u64_u32`, 55 moves).
  With two instructions per limb product (the `v_mad_u64_u32` and an add-with-carry that counts its carry), the products on pairs at even
  and at odd positions as on CUDA, and the counters of positions 3 to 8 in the high halves of the addends of the reduction (which were
  zeros), the multiplication goes from 226 to 176 instructions on gfx1030 (gfx1100 238 → 184, gfx906 225 → 172, gfx90a 229 → 175, gfx1200
  235 → 185). Loop of the kangaroo step 2394 → 2139 (gfx1030), 2667 → 2393 (gfx1100), 2451 → 2191 (gfx906); loop over the pairs of keys
  of the linear search (the hash is a call) 2253 → 1864, 2388 → 2013, 2280 → 1893. The linear kernel reaches 256 VGPRs and spills 32 to 80
  bytes once per group; the kangaroo kernel keeps 222 (234 on gfx906). `v_mad_u64_u32` is not a full-rate instruction, and the 50
  instructions removed per product are full-rate adds and moves: if it takes four cycles, as on GCN, the gain in time is about half the
  gain in instructions. The same form for the square does not pay (estimated ≈ 150 against the ≈ 150 measured for the rows: the doubling
  and the squares need chains of their own; the 108 of the table above subtracted a bench with two operands, the square loads one). The
  assembly is used on gfx9 to gfx12
  (`__GFX9__` .. `__GFX12__`, wave32 and wave64), never on pocl or other OpenCL compilers; `t_gpufe` runs the same product with the two
  instructions in C. No AMD GPU was at hand: a build that fails is repeated with `-DPZ_NO_ASM`, and an AMD device whose start-up self-test
  fails is tried again without the assembly before it is excluded (both paths checked on pocl with a kernel broken on purpose).
* **Tables and precomputation, evaluated** (the size and where each would be read from):
  * *The window of the linear search* (`i·G`, `HALF` × 64 bytes: 8, 16 or 32 KiB) is read at the same index by the whole warp: one
    broadcast L1 hit per 16-byte load. Per key the inversion costs ≈ 14,600/(2·`HALF`) instructions (57, 28 and 14 for the three windows)
    and the next centre ≈ 600/(2·`HALF`) (≤ 2.3). A window of 1024 would save ≤ 8 per key (0.3 %) for twice the private array (32 KiB
    per work-item); the tuner already measures 128, 256 and 512.
  * *SHA-256 and RIPEMD-160 constant words*: `hash160_w2` has no load from the constant bank and 700 `SHF.L.W` plus 155 `LEA.HI` (a
    rotation and an addition), the 855 rotations of the two compressions: the padding, the length and the schedule terms of constant
    words are folded by the compiler, and only word 0 depends on the parity (one `LOP3`). Every other word depends on x, which differs
    from key to key: nothing to keep between keys.
  * *The jump table in shared memory*: 272 entries × 64 bytes (x and y; the distances only serve the replay) = 17 KiB per block, 68 KiB
    per SM at four blocks of 128 (sm_86 has 100 KiB; sm_75's 64 KiB would need blocks of 256). The instructions stay (two fewer per step
    with 32-bit addresses). What changes is the work of L1: the 6 `LDG.128` of a step read 32 different lines per warp (up to 192 L1
    wavefronts per warp and step), against ≈ 10.2 wavefronts per `LDS.128` with the entries stored chunk by chunk (2.55-way bank
    conflicts in each quarter warp, simulated), ≈ 61 per step. Four warps of steps take ≈ 1,360 issue cycles of an SM and ≈ 1,000 of
    L1 today (with the private arrays): L1 is busy but is not the limit, and on the reference card the step is limited by DRAM (≈ 226
    bytes per step). It can matter only where the herd fits in L2: not implemented, a GPU is needed to know.
  * *Per-jump constants*: the step uses `jx` and `jy` only in subtractions from x and y; `−jx` or `−jy` stored would make additions of
    the same cost. *DP test*: two `LOP3` and a compare. *Batch-inversion tree across rounds*: every denominator changes every round.
  * *Starting points*: kangaroo starts and restarts are computed on the host (comb of `ec_mul_g`). The linear kernel computes its centre
    `P0 + gid·D` by double-and-add, ≈ 100,000 instructions per warp and launch (estimated), ≈ 2.4 per key with the 41,000 keys per work-item of a
    0.35 s launch on the reference card; a table of the 32 points `2^b·D` (2 KiB, uniform) would remove the doublings, ≈ 1.4 per key
    (0.05 %). Not done.
* **Tried and left out:** x passed to the hash by value instead of through the stack (8 `LDL` in `hash160_w2`): the callee loses 9
  instructions, the caller gains moves, −6 per key on sm_86, the m4 spill grows from 4 to 28 bytes, and clang then inlines the hash several
  times on AMD (15,602 instructions in the linear kernel against 4,321); the history entry addressed from a pointer per round (one
  `IMAD.WIDE` instead of seven 64-bit instructions): +3 moves and +3 uniform instructions, step 1132 → 1134; f and g of the inversion on
  fewer limbs as they shrink (libsecp256k1's variable-time functions do it): simulated on 300 warps, the slowest lane of a warp needs
  on average 4.9 limbs over its 19 rounds, so the update of f and g (≈ 85 instructions per round, d and e ≈ 115) would lose ≈ 45 %,
  ≈ 700 per inversion, 11 per step at NK = 64 (0.8 %) and 44 at NK = 16, ≈ 550 with three fixed sizes: it needs a warp vote on CUDA and
  diverges on OpenCL, not done; one inversion per warp (the products of the 32 work-items combined with shuffles): the inversion already
  runs once per warp for its 32 lanes, so only the wait for the slowest lane would go (12.6 → 9.8 turns per round, ≈ 3,000 instructions)
  against ≈ 1,550 for the prefix and suffix products across the warp, 1.6 % at NK = 64, not done; on CUDA, the `SEL` and the move (≈ 12
  per product) where a row chain carries into an occupied pair: counting the carries elsewhere, as on AMD, costs the same 12.
* **Need a GPU:** the time of the AMD product (and of the assembly on the drivers that compile OpenCL on Windows), the jump table in
  shared memory.

**After 1.17.0: the two integer pipes of the linear kernel.** Same tools as above (NVRTC 12.0 and `ptxas` for sm_86, the executed path
of the loops counted on the `nvdisasm` output; sm_75 and sm_89 within a few instructions; AMD ISA from clang 18) and the same checks (the
self-test and planted-key hunts on the simulated CUDA driver at windows 128, 256 and 512 and with the tuner measuring all six variants,
on pocl at windows 128 and 512 and for the kangaroo; `tests/t_gpufe.c`; `make test`, the end-to-end tests, clang, MinGW-w64 under Wine).
The arithmetic on the device itself is still to be timed on a card.

* **The model, checked against the 793 M keys/s of 10.5.** On GA10x an SM partition issues one warp instruction per cycle, to pipes 16
  lanes wide: the ALU pipe runs `IADD3`, `LOP3`, `SHF`, `LEA`, `SEL`, `PRMT`, `ISETP`, the FMA pipe `IMAD` in all its forms (`IMAD.WIDE`,
  `IMAD.X`, `IMAD.MOV`, `IMAD.IADD`), and a warp instruction holds its pipe for two cycles. With A and F the instructions per key on the two
  pipes and O the rest (loads, stores, calls, branches), the 32 keys of a warp take at least **T = max(2A, 2F, A + F + O)** cycles of their
  partition. 1.17.0 (`linear512m4`, the variant the tuner chose on the A2000): A = 2249, F = 515 (301 of them `IMAD.WIDE`), O = 27, so T =
  4499 cycles with 62 % of the issue slots in use, and the FMA pipe busy a quarter of the time. The measured 793 M keys/s are 6100 cycles
  per 32 keys per partition at the 1.89 GHz of the probes (80 partitions), 74 % of T: the ALU pipe is the bound, with the rest in
  dependency stalls, or in a clock below the probes' under a sustained load. The probes of `bench` fix the two rates (`mad.lo` 2.42 T/s,
  three-input add 2.52 T/s: the same 16 lanes) but not what an `IMAD.WIDE` costs: `mad.hi` runs at 1.00 T/s, 2.4 slots of the pipe, so the
  budget below is given with `IMAD.WIDE` at 1, 2 and 2.4 slots. 10.5 counted this idea in instructions (+14.5 % for all the adds of both
  hashes) and dropped it; in the time of the two pipes the same move is -8 %, and the forms below reach -20 to -23 %.
* **Additions on the FMA pipe.** `mad.lo.u32 d, a, one, b` with `one` a `__constant__` variable (`pz_one`, pz_cuda.h): neither NVRTC nor
  `ptxas` folds it (the host could overwrite the constant bank), so the add is an `IMAD` whose multiplier is read from c[3][0] as an operand,
  while `mad.lo` by the immediate 1, or by a `const` variable, goes back to `IADD3` (the finding of 10.5). A three-input sum costs two `IMAD`
  where one `IADD3` did, so every sum was placed by hand (PZ_ADDF, h_sum and rmd_sum in pz_common.cl):
  * SHA-256: h + K + w (off the chain of the state), + Ch, + Σ1: four `IMAD`; e = d + t1 one (`ptxas` already had it on `IMAD.IADD`); a = t1 +
    (Σ0 + Maj) two; the schedule word three. The constant words 9 to 15 and the IV of the first rounds must keep folding: the sums take
    compile-time flags that say which terms vary, and the constant ones are added as one immediate (without the flags the compiler could not
    see through the asm: 87 `ISETP` and 140 instructions more per key).
  * RIPEMD-160: the round constant goes into the message word ahead of the step (one `IMAD` with the constant as immediate, 125 per key: the
    word is used once per round, so it is the same number of additions as before, off the chain of the state), a + f + (m + K) two `IMAD`;
    the rotate-and-add stays the `LEA.HI` of `ptxas` (a `SHF` and an `IMAD` would not lower the ALU count); the five output sums `IMAD`.
  * Field: the carry out of an addition chain as `madc.lo.u32 d, x, zero, 0` (= CF, an `IMAD.X` with a second constant `pz_zero` as its
    operand), where `addc.u32 d, 0, 0` was a `SEL`: 14 per product, one or two per addition (ADDC_CF). The normalization of x3
    (`fe_normalize_fma`, the linear kernel only): the eight selects as `mad.lo` under the carry predicate, in one asm block. The parity of
    y: the carry chain of the normalization and one `LOP3` (`fe_parity`), no select. **Not the borrow of a subtraction.** The first test
    build took it the same way (`madc` after `sub.cc`/`subc`), and the A2000 failed the start-up self-test on `fe_sub(b, a)`, the case that
    borrows, while the simulated driver and pocl had passed: PTX defines CC.CF as the borrow after `sub.cc`, but `ptxas` implements the
    subtraction as the addition of the complement and carries the inverse of the borrow in its predicate, which `subc` consumers get
    inverted back and `madc` does not. The SASS says it: `IADD3.X R44, P0, R19, ~R8, RZ, P0` then `IMAD.X R45, R44, c[0x3][0x4], RZ, P0`, so
    the "borrow" read was 1 − borrow (the 1.17.0 form `subc.u32 w, 0, 0` compiles to `IMAD.X R9, RZ, RZ, -0x1, P0`, that is −borrow, and
    then `w & 1`). The simulated CUDA driver compiles the kernel sources as C and never executes PTX, so no test here can catch a wrong
    reading of PTX or `ptxas` semantics: PTX inline assembly is checked on the SASS and then on a card. The borrow-outs are back to
    `subc` + `and` (two instructions, one of them on the ALU pipe, 8 per pair of points); none of the forms of 1.14.0 to 1.17.0 mixes the
    two chain types (checked over the 179 chain instructions of the 1.17.0 field code: every `addc`/`madc` follows an `add.cc`/`mad.cc`,
    every `subc` a `sub.cc`), which is why they passed on the card in the 1.15.0 bench. The IMAD.X carries and the asm normalization are
    compiled into the linear and test modules only (`-DPZ_FMA_CARRY` in modules.txt): with them in the kangaroo modules `ptxas` spilled
    `kang64b` (48 bytes) and the step grew by 7 to 35 instructions per loop, for a kernel the memory bounds, so their PTX is that of 1.17.0
    but for the two constant declarations.
* **Budget per key** (sm_86, `linear512m4`; F counts `IMAD.WIDE` once; the pair loop is C ± i·G for one i, the forward loop one denominator):

  | phase                           | 1.17.0: n / A / F / O     | now: n / A / F / O        |
  |---------------------------------|---------------------------|---------------------------|
  | `hash160_w2`, per key           | 2030 / 1911 / 110 / 9     | 2393 / 1453 / 931 / 9     |
  | pair loop, per 2 keys           | 1338 / 610 / 701 / 27     | 1340 / 522 / 791 / 27     |
  | forward loop, per 2 keys        | 183 / 66 / 109 / 8        | 183 / 58 / 117 / 8        |
  | `fe_inv`, per 1024 keys         | 587 / 266 / 282 / 39      | 592 / 288 / 268 / 36      |
  | **per key**                     | **2791 / 2249 / 515 / 27** | **3155 / 1743 / 1385 / 27** |

  T goes from 4499 to **3487** cycles per 32 keys per partition with `IMAD.WIDE` at one or two slots (2F = 2771 and 3372 against 2A = 3487:
  the two pipes are nearly even at two slots) and to 3608 at 2.4 slots (then the FMA pipe is the bound): **-20 to -23 %**, the issue slots
  at 90 % of T (99 % at two slots), which is why the conversion stops here. In the hash 927 `IMAD` replace 457 `IADD3` and 108 `IMAD.IADD`; what
  stays on the ALU pipe is the floor of the expression on this ISA: 788 `SHF` (the rotations, the three
  shifts of each sigma), 492 `LOP3`, 154 `LEA.HI`, 8 `PRMT`. The field part keeps 288 ALU instructions per key, 200 of them `IADD3.X`: the
  E + O sum and the two chains of the reduction (35 per product) and the carry chains of the additions; the products are 263 `IMAD.WIDE`
  per key (three multiplications and one square) plus 37 in the forward pass; the inversion is 0.6 instructions per key at window 512.
  Registers: 172 for the free variants (162 on sm_75), 128 for the capped ones with **no spill** on sm_86 and sm_89 (4 bytes before), 12 bytes
  on sm_75 (8 before), stored once per group outside the loop over the keys; the kangaroo modules are unchanged. OpenCL: PZ_ADDF
  is a plain addition, and clang produces the same code as before for the linear kernel (4321 instructions on gfx1030, 4540 on gfx1100,
  4444 on gfx906).
* **Expected on the A2000.** If the 1600 cycles per warp between T and the measurement stay what they are, 952 M keys/s (+20 %); if they grow
  with the instruction count, 915 M (+15 %); if the kernel keeps 74 % of T, 1030 M (+30 %). `pzsolve bench --gpu-only` prints the keys/s
  of the linear kernel (the line `linear search`); the tuner picks the window again on the first run of the new version.
* **Tried and left out, or left for a card:**
  * the two shifts of the sigmas (87 `SHF.R` per key) as `mul.hi` by 2^29 and 2^22 from the constant bank (`IMAD.HI`): -87 on the ALU pipe, but
    at 2.4 slots each the FMA pipe becomes the bound and T does not move (3607); -5 % if `IMAD.HI` took one slot. A card decides;
  * a rotation as `IMAD.WIDE` by 2^n (the two halves of the product are the two parts of the rotation) and an `IMAD` to join them: 3 to 4
    FMA slots for one `SHF`; about 40 of the 788 rotations would balance the pipes again, for a gain under 2 %, with 64-bit pairs in a
    kernel at 128 registers. Not done;
  * the kangaroo with the IMAD.X carries and the asm normalization: around its arrays in local memory the asm block costs 8 scalar `LDL`
    and `STL` per step (1199 against 1191), and the carries alone made `ptxas` spill `kang64b` and grow every variant (above): it keeps the
    forms of 1.17.0;
  * the borrow of a subtraction on the FMA pipe without reading the predicate after `sub.cc`: `subc.u32 m, 0, 0` gives −borrow (an `IMAD.X`
    already), and the correction can be written as an addition chain of m·977, 2m, m, .., m (the two's complement of borrow·(2^32 + 977)),
    whose carry out `madc` may read: one ALU instruction less per subtraction (16 instead of 17 for `fe_sub_norm`, about 4 per key) for a
    second form of every correction. Not done;
  * `w[8]` of the SHA message as a multiply-add (`x·2^24 + 0x800000`): `ptxas` emits the same `LEA` either way;
  * the ALU floor of the field part: `x3 = λ² − xs` is an `fe_sub` and a normalization (51 ALU instructions per point); the subtrahend added
    into the product before its reduction, as the IFMA lanes do since 1.17.0, would need a three-input carry chain, which PTX cannot
    express (`IADD3.X` has two carries in SASS, `ptxas` never fuses two chains into it); the E + O sum of the product (15 `IADD3.X`) is the
    price of the aligned `IMAD.WIDE` pairs, and reducing E and O separately costs more chains (27 against 24).

**After 1.19.1: energy per key on a card held back by its power limit.** On the A2000 the two-pipe kernel of 1.18.0 gained 11 % (793 to
882 M keys/s) where the issue model said 20 %, and by the issue model the card ran at about 1.3 GHz sustained against the 1.89 GHz of the
probes: it is held by its power limit, so what bounds the speed is the energy of a key, not its cycles alone. The model is
`tools/research/sass_energy.py`: the executed path of the loops over the keys, from the SASS of `ptxas` (sm_86, NVRTC 12.0), counted by
class and by operand traffic (register reads, reads served by the operand reuse cache, which `nvdisasm` marks `.reuse` from the control
bits described by Jia et al., *Dissecting the NVIDIA Volta GPU Architecture via Microbenchmarking*, 2018; constant-bank and uniform
operands; register writes), the cycles of the issue model above (the mean of 16 staggered starts of the warps: one start alone moves the
result by up to 2 %; an `IMAD.HI` at the 2.4 issue slots of the probes, an `IMAD.WIDE` at 1, 2 or 2.4), and the 32 bytes per key of the
prefix products that go to the video memory. Energy per key = Σ (issue + reads + writes + datapath + L1) + cycles × a per-cycle term +
bytes × a per-byte term, in units of one warp-wide 32-bit register read. The weights are not published for this chip, so each is a range
and the results are over 2000 random draws: issue 0.3 to 1; a write 1 to 1.5; a reuse-cache read 0.1 to 0.25; a constant operand 0.05 to
0.2; the datapath of an add, logic op or shift 0.02 to 0.1 and of a 32×32-bit multiply 0.3 to 1 (Horowitz, *Computing's energy problem*,
ISSCC 2014, 45 nm: a 32-bit add 0.1 pJ, a 32-bit multiply 3.1 pJ, a 64-bit read of an 8 KB SRAM 10 pJ, so on 32 lanes the add is a few
hundredths of a 1024-bit register read and the multiply about half of one), an `IMAD` by one 0.05 to 0.3 (one row of partial products
toggles), an `IMAD.WIDE` 0.5 to 1.6, a load or store through L1 2 to 8, a byte of GDDR6 0.7 to 3 (5 to 8 pJ per bit; O'Connor et al.,
*Fine-Grained DRAM*, MICRO 2017, give about 14 for GDDR5 and 4 for HBM2). With them a multiply-add costs 1.4 times an add per instruction, as
Arafa et al. measured on Volta and Turing (*Verified instruction-level energy consumption measurement for NVIDIA GPUs*, CF 2020: mul/mad
against add 0.8 to 1.5 by their two methods). The per-cycle term (clock tree, leakage, the parts of an SM that run whatever is issued) is the unknown that each
draw fits to the measured 793 → 882, for a voltage that follows the clock as V² ~ f^ε, ε from 0 to 1.

* **What it says of 1.17.0 and 1.18.0.** Per key the two-pipe form has 3154 instructions against 2790, 6487 register reads against 5816,
  3435 writes against 3071 and 769 constant operands against 8: the energy of its instructions and DRAM is **11 % higher** (8 to 14 % over
  all the draws), whatever the weights (the classes move together: 927 `IMAD` by one replace 457 `IADD3` and 108 `IMAD.IADD`, each with
  about the same operands). A card bound by that energy alone would have lost 10 %; it gained 11 %. The model reproduces the measurement
  only if most of the energy goes with the cycles: **41 to 70 % of it with `IMAD.WIDE` at one issue slot, 94 to 97 % at two**, and with
  `IMAD.WIDE` at 2.4 slots (as slow as `IMAD.HI`) no draw can (the cycles alone give 4880 → 4466, +9.3 %, less than measured), so it takes
  fewer. The DRAM is 1 to 13 % of the energy. So on this card the part that goes with the cycles is 0.7 to 30 times the energy of the
  instructions themselves, and the ranking of variants is the ranking of their cycles, corrected by their instruction energy; the class weights decide
  nothing below.
* **Kept** (the issue model, cycles per 32 keys per partition with `IMAD.WIDE` at one and two slots; keys/s at the same power against
  `linear512m4` of 1.19.1, 3782 and 4175 cycles):
  * the first denominator and the centre out of the loop over the pairs, so that `ptxas` no longer copies the inverse and the centre
    ahead of the tests on the index: 3154 → 3140 instructions per key (moves 56 → 47), 3770 and 4145 cycles, **+0.3 to +0.7 %**; the
    capped variants spill 88 bytes outside the loops over the keys, the free ones take 188 registers (two blocks per SM, as before);
  * `linear512m5`: the same kernel capped at 96 registers, 5 blocks of 128 per SM (5 warps per partition, where the four of the 128-register
    cap stay in phase and leave 16 to 25 % of the issue cycles idle with a pipe busy): 240 bytes of spill, outside the loops but for 2
    stores per key, 3710 and 4061 cycles, **+1.5 to +2.7 %**;
  * `linear512m4p`: the prefix products of the batch inversion kept for every other denominator (`-DMC=2`): the backward pass takes two
    denominators per turn, r = inv · p with p the kept prefix, and the inverses are r · d(m−1) and r · d(m), five products where four did
    (+0.25 of a multiplication per key), while the private array halves (8.7 KiB per work-item at window 512) and so does its traffic (16
    bytes per key). 3158 instructions per key, 3735 and 4137 cycles, energy of the instructions and DRAM -4 % (-9 to -1), **+1.0 to +2.2 %**
    (+0.9 to +3.9 % over the draws and both slot counts). It and the 96-register cap do not combine: 740 bytes of spill loads, inside the loop.
  The tuner measures the two new variants with the six others; the self-test checks all eight. OpenCL keeps its four (the kernel source
  takes `-DMC=2` too; pocl runs it bit-exact).
* **Measured in the model and dropped.** The carries and the normalization back on the ALU pipe (the field phase is the one bound by the
  FMA pipe): +0.0 to +1.1 %, -0.3 to +0.4 % against the kept form, not a variant. The 87 shifts of the SHA sigmas per key as `IMAD.HI` by
  2^29 and 2^22 from the constant bank: the ALU pipe loses 87 instructions, the FMA pipe gains 209 slots, -0.2 to -2.0 %. The window table
  in constant memory: its loads are 3 of the 15 loads and stores per key, under 1 % of the instruction energy at any weight. A window of
  1024: 0.6 instructions per key. The sums of the hash back on `IADD3` where only the balance of the pipes put them on `IMAD` (one
  instruction and its operands fewer per sum): the cycles they add cost more than the energy they save as soon as the per-cycle term is
  above about 40 % of the energy, which the measurement says it is.
* **Tensor cores** (int8 `IMMA`, `mma.sync.m16n8k32.u8`). A product maps to a matrix product only when one operand is shared by the
  32 keys of a warp. In the step none is: the table points (shared by the warp) enter only through additions and subtractions (`d = x(i·G)
  − x(C)`, `y(i·G) ∓ y(C)`), the centre is per work-item, and λ, λ², λ·(x(C) − x3), the inverse and the prefix products all depend on the
  key. Rewriting λ = y(i·G)·im ∓ y(C)·im gives one product per key a shared operand for one product more. Even then, 32 field products of
  32 byte-limbs by a shared one are 16 `mma.m16n8k32` (about 128 cycles of the tensor pipe of a partition), and the 63 columns of 32-bit
  sums per key then need the carries and the reduction (about the 121 instructions of the `IMAD.WIDE` product itself) and the moves
  between the fragment layout and the lanes. The share of the work that could map is 0 today and one product in five or six with the
  rewrite, at a loss: not done.
* **Expected on the A2000** (882 M keys/s with `linear512m4` in 1.19.1): `linear512m4` 885 to 888, `linear512m4p` 890 to 900,
  `linear512m5` 895 to 905, so about **900 M keys/s** once the tuner has picked. The gains are of the order of the scatter between two
  runs of `bench` on a laptop, so the variants are worth timing in alternation, each forced with `PZ_GPU_HALF` (262656 = 512 with the
  128-register cap, 328192 = 96 registers, 786944 = pairs) and `pzsolve bench --gpu-only`, twice in turn, with
  `nvidia-smi --query-gpu=clocks.sm,power.draw,power.limit,clocks_throttle_reasons.active --format=csv -l 1` alongside; then
  `pzsolve bench --gpu-only --retune` to let the tuner choose. If the clock under the variant that wins is higher at the same power, the
  per-cycle reading above is confirmed; the three figures also bound how many issue slots an `IMAD.WIDE` takes (the 96-register cap gains
  more at two slots, the pairs at one).
* **Measured on the A2000, and dropped.** Each variant forced in turn, `bench --gpu-only`, keys/s: 1.19.0 `linear512m4` 870 to 881 (four
  runs); with the peeled loop `linear512m4` 857, `linear512m5` 862, `linear512m4p` 857, `linear256` 844 to 852. The peel alone cost
  `linear512m4` 2.8 % on the card, although it saves 14 instructions per key in the SASS, and neither energy variant won it back
  (+0.6 % and 0 % over the peeled `linear512m4`). The tuner then took window 256 (the 512 variants were within 1 % of it), and a campaign of
  puzzle 71 with the same 15 threads and GPU ran at 785 M keys/s on the GPU against 814 M with 1.19.0. The model above weighs instructions
  and register traffic; what the peel moved is the shape of the loop (the tests on the index leave it, the body grows by one copy of the
  pair), and on this card that matters more than 14 instructions. The kernel source and the six modules are back to those of 1.19.0
  (identical PTX but for the numbers in the callseq comments), and the two variants are removed. The energy model stays as a tool; its
  ranking of variants this close is not reliable.

**After 1.19.1: the block herd, one inversion per block.** The idea (chosen by the owner): instead of one batch inversion per work-item over
`NK` = 64 to 256 walkers kept in device memory, a few walkers per work-item kept in registers and one inversion shared by a whole block of 128
work-items, so that a step reads and writes no walker state in device memory. Built as a kernel variant (`kang4g`, `NK` = 4, suffix `g` of
`PZ_GPU_NK`, "NK 4 block herd"), forced with `PZ_GPU_NK=4g` (the CUDA tuner measured it in the test builds and never took it); the counts are SASS from NVRTC 12.0 and `ptxas` for sm_86 on the executed path (the
scheduling control words that `ptxas` writes into every instruction give the latencies), the speed is a model: no GPU was at hand.

* **The kernel.** Each work-item keeps x, y, flags and tags of its 4 walkers in registers for the whole launch (the loops over them are
  unrolled, so the arrays have constant indices). A round: the 4 denominators and their prefix products; the product of the work-item into
  shared memory; barrier; 32 work-items (one warp, a different one each round) multiply the 4 products of a column (work-items c, c + 32, c +
  64, c + 96), invert the 32 column products together (one `fe_inv` for the 32 lanes) and work back to the inverse of each factor (Montgomery's
  trick on the column: 3 products up, 6 down); barrier; each work-item reads its inverse and steps its walkers as before. Two barriers and 7
  KiB of shared memory per block. The walk, the jump table, the history (16 bits per step, replayed at the end of a launch and at every
  distinguished point), the state layout and the events are those of the other kernels with `NK` = 4: the host code is the same, and the
  self-test compares the kernel with the CPU bit for bit. The one difference is inside a kernel launch: a work-item whose product is zero (a
  walker on its jump point, never seen) restarts its 4 walkers and then only takes part in the inversions.
* **Capacity.** `ptxas` gives 250 registers, no spill, for `NK` = 4 (241 on sm_75, 250 on sm_89): two blocks of 128 per SM, **1,024 walkers per
  SM, 20,480 on the A2000**. `NK` = 2 and 3 take 194 and 216 registers (still two blocks), 5 spills 24 bytes at the 255 cap, 6 spills 256 and 8
  spills 952. Shared memory would hold more walkers (124 bytes each with their prefix product: two more per work-item would be 31 KiB per
  block), but the 48 KiB of static shared memory per block bound it, and the walkers held are not what limits the kernel (below).
* **Device memory.** Per step the 2-byte history entry, written in the round and read back by the replay at the end of the launch, and the
  128-byte state of a walker read and written once per launch: **≈ 4 bytes per step instead of ≈ 215**. The jump table (21 KiB) stays in L1.
  Shared memory: 64 bytes per work-item and round (its product out, its inverse back) and ≈ 0.5 KiB per column for the inverting lane, ≈ 50
  bytes per step, about 1 byte per cycle of an SM that moves 128; the `LDS.128`/`STS.128` at a 32-byte stride are 2-way bank conflicts, four
  instructions per work-item and round. Launches: with 20,480 walkers a history of 256 rounds would be a 10 ms launch, so the block herd has up
  to 16,384 rounds (`KANG_HIST_MAX_BLOCK`, within the eighth of the device memory: 8,192 on the A2000, 335 MiB, ≈ 0.4 s at the expected speed),
  but at most 8 · 2^dp-bits of them past 256, because each distinguished point replays the history of its walker since the start of the launch.
* **Instructions** (sm_86, executed path of a round, `NK` = 4): **3,766 per work-item for 4 steps, 941 per step** (1,812 ALU pipe, 1,868 FMA
  pipe, 86 other, no `LDL`/`STL`; the loop of `kang64` is ≈ 1,130 with its arrays in device memory); the inverting warp adds 1,230 (the 9
  products of the column, the shared loads and stores, the call) and `fe_inv`, ≈ 14,450 executed per warp (80 + 19 rounds × (20 + 12.6 turns ×
  33 + 313) + 124, the counts of section 5 after 1.13.0). One block-round is 16 warp-steps: **941 + 15,680 / 16 ≈ 1,920 instructions per step,
  against ≈ 1,300 for `NK` 128 quads**. The integer ceiling at the `mad.lo` rate of the probes drops from 1.86 to 1.26 G steps/s.
* **Latency, which decides.** A block cannot start round r + 1 before its inverting warp has finished round r, and the inverse is a serial
  chain: the stall counts of the round sum to 1,379 cycles before the first barrier, 35,203 for the inverting warp between the barriers (32,000
  of them `fe_inv`: 170 + 19 × (45 + 12.6 × 83 + 571) + 226; a turn of the divstep loop is 33 instructions and 83 cycles) and 8,094 after the
  second, **≈ 44,700 cycles per round for one warp alone**. The issue model of `tools/research/kang_block_sched.py` (one instruction issued per
  cycle and partition, each pipe held two cycles per warp instruction, the stall counts and scoreboards of `ptxas`, the barriers of each block;
  two blocks per SM, warp w of a block on partition w, IMAD.WIDE at one slot): **52,500 cycles per round for one block alone, 54,100 for two
  blocks whose inverting warps fall on different partitions, 61,100 when they fall on the same one** (blocks b and b + 20, co-resident on a
  card of 20 SMs, invert on the same partition in the same rounds with the rotation (round + block) mod 4); with IMAD.WIDE at two slots 57,900
  and 69,300. The issue slots are 30 % busy: the kernel is bound by the latency of the inversion, not by its instructions or by memory. 1,024
  steps per 54,000 to 61,000 cycles per SM: **0.44 to 0.49 G steps/s at the 1.3 GHz the A2000 sustains under load, 0.63 to 0.72 G at the 1.89
  GHz of the probes, against 0.91 G measured for `NK` 128 quads.** To reach 0.91 G with a round of 54,000 cycles an SM would need ≈ 1,900
  walkers on chip; registers and shared memory together hold at most ≈ 1,500, and the extra walkers lengthen the round (6 walkers per
  work-item: ≈ 0.65 G, estimated from the same stall counts). A shorter round needs a faster single inversion (the divstep loop runs at 2.5
  cycles per instruction for one warp) or more independent inversions in flight per SM, which is what the per-work-item batches of the present
  kernels are.
* **What differs from the analysis of 1.7.1** (section 8.4, "why keeping the herd in shared memory or in L2 does not pay"). It counted shared
  memory only (≈ 800 walkers per SM at ≈ 130 bytes): the registers (256 KiB per SM) hold more, 1,024 walkers with no private array in device
  memory. It had the inversion run by a single thread of the block, cost 153/W of the step for W walkers per block (10,500 instructions per
  inversion, 2,200 per step); here 32 lanes invert 32 column products for the price of one warp-inversion, which is the same stream cost, so
  the form of that estimate holds, but its numbers moved against the idea: the step is now 941 instructions without memory traffic and the
  inversion 14,450 as executed, so with W = 512 the inversion adds 104 % of the step instead of 30 %. And it did not count the latency, which
  bounds the block herd before the instructions do. What it gains is real: the device memory traffic of a step goes from ≈ 215 to ≈ 4 bytes, so
  the block herd can pay only on a card whose bandwidth per SM-cycle is well below the A2000's (6.3 bytes at 1.3 GHz) and which has no L2 large
  enough for the cache-sized herds of 1.7.1. The tuner measures it, so such a card picks it without a rule.
* **Herd size, distinguished points and the constant.** 20,480 walkers instead of 1.3 million: `dp-bits` follows from `KANG_DP_OVERHEAD`
  (puzzle 135 with the card alone: 45 instead of 39; the overhead K · 2^dp-bits is the same share of √W by construction), the table holds the
  same number of points, and 8.12 found nothing lost to the walks from 2^14 walkers up (where memory sets `dp-bits`, a smaller herd loses
  less).
* **Checks.** The self-test runs the block herd with one whole block of 128 work-items and two of the next (with the 4 work-items of the other
  variants every row of a column but the first would be the 1 of the work-items past the end, and a wrong product in the down pass went unseen:
  found by mutation). The simulated CUDA driver runs a kernel with barriers a block at a time, its work-items in threads that hand control to
  one another at each barrier (coroutines would make AddressSanitizer print a warning into the output of the ASan build that loads the driver),
  and fails a launch whose barriers are not reached by every work-item; the OpenCL runtime launches a kernel built with `reqd_work_group_size`
  in groups of that size. Mutations of the barrier and of the column products are caught on the simulated driver and on pocl; planted keys of
  puzzles 44 and 50 are found with `PZ_GPU_NK=4g` on both.
* **On the card**: `PZ_GPU_NK=4g pzsolve bench --gpu-only` (the line `kangaroo ... NK 4 block herd`), expected 0.44 to 0.49 G steps/s if the
  clock stays near 1.3 GHz, up to 0.7 G if the lighter load lets it rise; `pzsolve bench --gpu-only --retune` should keep `NK` 128 quads. It
  did (910 to 916 M steps/s with `NK` 128 quads), so the tuners no longer measure the block herd: it stays a forced variant. Not
  done: choosing the inverting warp from `%warpid` so that co-resident blocks never invert on the same partition (61,100 → 54,100 cycles, +13 %
  for this variant, with inline assembly).

**After 1.19.2: the bytes of a kangaroo step.** The kernels with the walkers in private arrays (`NK` 16 to 256, plain, pairs and quads) are
bound by the traffic of those arrays to the device memory (8.4), and the figure the model used for it, ≈ 215 bytes per step, was a count by
construction from 1.7.0. This is the count from the SASS: `tools/research/kang_bytes.py` rebuilds the executed path of one turn of the loop
over the walkers (as `kang_block_sched.py` does for the block herd: the branches that skip a block with an atomic, a restart or a distinguished
point, are taken; the rest is the common path) and sums its `LDL` and `STL` by width and by the array they address (from the offsets in the
stack frame), the `STG.U16` of the history and the `LDG.128` of the jump table. NVRTC 12.0 and `ptxas` for sm_75, sm_86 and sm_89; no GPU was
at hand, the one measurement below is the 911 M steps/s of 1.19.2 on the A2000. The arrays are local memory, which the hardware interleaves so
that one `LDL.128` of a warp is 512 contiguous bytes: every access is coalesced, and none of it can stay in a cache (the private arrays of the
A2000's herd are 160 MiB per round against 2 MiB of L2).

* **One step of `NK` 128 quads in 1.19.2, sm_86** (per walker and round, the executed path; the same on sm_75 and sm_89): x read and written
  32 + 32, y 32 + 32, the stored products 8 + 8 (one of 32 bytes per block of four), the flags 7 read (4 in the prologue of the block, for the
  jump index of the denominator, and 4 again in the step by three walkers in four, since the quads kernel let `ptxas` reload them) and 4
  written, the tag of the point left 4 read and the two tags 8 written, the checkpoint tag and least tag 8 read (written only when they change:
  on checkpoint steps and while the least tag falls, under 1 byte per step), the history entry 2: **177 bytes**, of which 91 read and 86
  written. Plain `NK` 128: 222 (the products 32 + 32, the flags read once); pairs 192. Per launch, not per round: the state of a walker is
  read and written once (128 + 128 bytes; the records are 128 bytes apart per walker and `NK` · 128 apart between the lanes of a warp, so
  each `LDG.32` touches 32 lines, but the 32 words of a record are read in sequence and the lines are used whole), the replay reads the
  history again (2 bytes per round) with the jump distances from L1, and the product and inverse pass through local memory around the
  `fe_inv` call (128 bytes per round per work-item). With the 128 rounds a launch runs on the A2000 (the history of 1.3 million walkers
  takes the eighth of its 4 GB at that depth) this is 4 more bytes per step: **181 for the quads**, and 911 M steps/s × 181 bytes = 165 GB/s,
  the copy bandwidth `bench` measured on that card. The kernel runs at the bandwidth of the card; the "ceiling" of 769 M steps/s that 8.4
  computed with 215 bytes was the plain kernel's, and nothing had to come from the L2. What the L2 does serve: the first walkers of a round
  read the x, y and product the previous round wrote moments earlier (the pass turns around), and at 160 GB/s a line stays in 2 MiB about 12
  µs, which is two steps of one work-item: about 1 % of the reads. The jump table (21 KiB) is 120 bytes of `LDG.128` per step from L1.
* **Kept: the metadata written and read once.** The two tags one and two steps back were both written at every step (an 8-byte store); the
  tag two back is used only by the next jump choice, which the step that computes it knows (it is the tag the step itself reads), so it
  travels no further than the registers, and the last round of a launch stores it for the host alone (a launch that stops early because a
  product was zero leaves the value the launch began with, for walkers the host restarts anyway: the self-test compares the end state bit
  for bit and passes on the simulated driver and on pocl). The quads kernel keeps the flags of its block of four in registers between the
  prologue and the steps (`BF`, 4 registers; the pairs kernels at the 128-register cap would spill them: there the step reads its word
  again, as before). The checkpoint tag and least tag are one 8-byte word, read where they are compared. Per step, executed path, sm_86:

  | kernel          | bytes 1.19.2 → now | instructions per step (loop) | registers, spill bytes |
  |-----------------|--------------------|------------------------------|------------------------|
  | `NK` 128 quads  | 177 → **170**      | 1161 → 1159                  | 216 → 230, none        |
  | `NK` 64 quads   | 177 → **170**      | 1163 → 1164                  | 216 → 222, none        |
  | `NK` 128 plain  | 222 → **218**      | 1082 → 1079                  | 128 → 126, none        |
  | `NK` 64 pairs   | 192 → **188**      | 1158 → 1157                  | 128, none              |
  | `NK` 128 pairs  | 192 → **188**      | 1161 → 1164                  | 128, none              |
  | `NK` 256 pairs  | 192 → **188**      | 1162 → 1161                  | 128, 48 outside the loop |

  sm_89 is the same to the instruction. On sm_75 the plain and quads kernels give the same bytes and registers as sm_86, and the pairs
  kernels at the 128-register cap, which spilled inside the loop before and after (an amount `ptxas` does not report: the values go to local
  memory before its allocation), move either way: `NK` 64 pairs 216 → 188 and `NK` 128 pairs 222 → 204 (fewer spills), `NK` 256 pairs
  212 → 222 (more). The `NK` 16 and 32 kernels of the cache-sized herds behave like their larger siblings (16: 222 → 218 with the 4 spill
  bytes gone; 32 pairs on sm_75 212 → 222). The block herd is untouched (its words are registers: 250 registers, no spill, as before). Round
  overhead per step unchanged: ≈ 65 instructions plus `fe_inv` (14,540 executed per warp) over `NK`. OpenCL: pocl runs every variant bit-exact;
  for gfx1030 clang 18 gives the same VGPR counts within 4 (the quads 250 → 254, no spill) and the same private loads and stores by count.
  **Expected on the A2000**, a model: at 181 → 174 bytes per step and the bandwidth it already reaches, 911 → ≈ 948 M steps/s (+4 %) with `NK`
  128 quads; the instructions per step do not move, so the energy of a step falls with its bytes. Measured below: the gain did not appear, and
  the form is now the variant "tags once" (`PZ_GPU_NK=128cm`) next to the kernel of 1.19.2.
* **Counted and dropped.**
  * *x kept across the turn of the pass.* The kernel has one pass per round since 1.7.0 (no second read of x), and the walkers whose state
    could stay on chip from the end of one pass to the start of the next are the few the L2 already serves (above). Shared memory would hold
    390 bytes per work-item at the 256 of the quads kernel, six walkers' x and y of 128: 3 bytes per step at best, for `LDS`/`STS` in the
    loop. Not done.
  * *Fewer metadata bytes than 24.* The tag one step back (4 read, 4 written), the flags with the jump index (4 + 4: the index is chosen in
    round r for the denominator of round r + 1 and read back in r + 1; choosing it again in r + 1 needs the tag two back, the same 4 bytes
    either way), the checkpoint tag and least tag (8 read, compared at every step; a shorter tag would change the walk, which the CPU shares).
    The kind could be derived from the walker's index, but it is one bit of a word that is needed anyway.
  * *Products every eighth walker* (an octets kernel, −8 bytes per step for 0.5 of a product more). Two forms: the one of the pairs and
    quads, with the x, denominator and suffix product of every walker of the block kept (192 registers of block state: spills); and a form in
    two halves, keeping x and the flags only, forming a denominator again from x and its jump point where it is used, the product of the
    second half first, then the suffix products of each half (10 products per 8 walkers against 6 for two quads, 104 registers of block state
    against the quads' 96): `ptxas` reaches the 255 cap and spills through the frame on sm_86, 223 bytes per step against 170. The register
    budget of a block is its `MC` x's plus its `MC` suffix products plus the step, and a step needs about 90: 8 walkers do not fit in 255.
  * *Longer launches.* The fixed cost of a launch is 256 bytes of state per walker and the first products; at 128 rounds (the A2000) it is 2
    bytes per step, 1 at 256, for 335 MiB more of history (an eighth of the card's memory is the rule in `gpu_kang_depth`). Not done.
  * *The history* is written as 64 contiguous bytes per warp and step and read the same way by the replay: 4 bytes per step, as designed.
  * *The sentinel product* (`TP[0] = 1`, 32 bytes written and read per round per work-item) and the product and inverse around the `fe_inv`
    call: 1.25 bytes per step at `NK` 128, a branch per block or a change of the call for them. Not done.

* **Measured on the A2000** (the owner, `bench --variants`, three turns, medians, steps/s; the kernels with the tags written once for every
  variant, as the branch then had them; the tuner's pick in every bench of 1.19.2 was `NK` 128 quads at 910 to 916 M, the only figure of that
  version, the other variants were never printed):

  | variant        | M steps/s | spread | bytes per step (SASS, with the launch) | implied GB/s |
  |----------------|-----------|--------|----------------------------------------|--------------|
  | `NK` 64 quads  | **904**   | 0.2 %  | 174                                    | 157          |
  | `NK` 128 quads | 716       | 0.2 %  | 174                                    | 125          |
  | `NK` 256 pairs | 850       | 0.0 %  | 190                                    | 161          |
  | `NK` 64 pairs  | 702       | 1.3 %  | 190                                    | 133          |
  | `NK` 128 pairs | 694       | 2.3 %  | 190                                    | 132          |
  | `NK` 256 plain | 742       | 0.5 %  | 221                                    | 164          |
  | `NK` 64 plain  | 635       | 0.2 %  | 221                                    | 140          |
  | `NK` 128 plain | 629       | 0.4 %  | 221                                    | 139          |
  | block herd     | 593       | 0.6 %  | 4                                      |              |

  So `NK` 128 quads lost 21 % against the kernel of 1.19.2 on the same card, in all three turns alike (the turns run the variants in order,
  reversed, in order: a drift of the clock would show in the spread), while `NK` 64 quads stands where 1.19.2's `NK` 128 quads stood, and
  the `NK` 256 kernels run at the copy bandwidth of the card (165 GB/s) where the `NK` 64 and 128 ones of the same form do not. The herds:
  `NK` 128 is 1.31 million walkers with a history 128 rounds deep, `NK` 64 is 655 thousand at 256 rounds, `NK` 256 2.6 million at 64, so a
  launch is 168 million steps in every case; the herd size, the depth and the measuring procedure (`kang_measure`: a random herd, launches
  doubled up to the depth, the best of about 1.5 s) are the same code as in 1.19.2.
* **The two kernels side by side in the SASS** (`NK` 128 quads, sm_86, `ptxas` 12.9, the executed path of one turn of the loop over the
  walkers, four steps; `tools/research/kang_bytes.py` and the control words of the instructions as `kang_block_sched.py` reads them). The
  candidates for the cause, each with its number:

  | proxy                                                         | 1.19.2 form | tags once |
  |---------------------------------------------------------------|-------------|-----------|
  | bytes per step (read + written)                               | 91 + 86 = 177 | 88 + 82 = 170 |
  | instructions per step (ALU pipe, FMA pipe, other)             | 1161.2 (523.0, 599.5, 38.8) | 1159.2 (521.2, 599.0, 39.0) |
  | instructions of the turn, sum of the stall counts             | 4645, 12246 | 4637, 12124 |
  | `LDL` and `STL` of the turn                                   | 33, 32      | 30, 36 (4 `STL.64` became 4 `STL` and 4 predicated `STL` that run in the last round only) |
  | scoreboard waits on loads per turn (loads per wait)           | 43 (30 of one load, 11 of two, one of four, one of seven) | 40 (27, 11, 1, 1) |
  | distance from an `LDL` to its first use, median               | 119 instructions, 340 stall cycles | 121, 327 |
  | registers, spill (sm_75 / sm_86 / sm_89)                      | 218 / 216 / 216, none | 230 / 230 / 230, none |
  | registers with `ptxas` 12.0, 12.2, 12.4, 12.6, 12.8, 13.0 (sm_86) | 214, 214, 216, 216, 216, 216, no spill | 232, 232, 232, 230, 230, 230, no spill |
  | stack frame; `LDL`/`STL` outside the loop                     | 13,888 bytes; the prologue, the epilogue and the 64 bytes passed around `fe_inv` | the same |
  | `kang_emit_dp`, `kang_replay_state`, `fe_inv` (called)        | 589, 582, 583 instructions, no local access but `fe_inv`'s 8 + 8 | 590, 572, 584, the same |

  1. *Registers.* 230 is below the 255 the launch bound allows (two blocks of 128 per SM either way: 65,536 / (232 × 128) = 2.2, and the
     allocation is in units of 8), nothing spills in any function, with any `ptxas` from 12.0 to 13.0, so the driver's compiler is unlikely to
     have spilled either; the rare paths (the distinguished point, the restart, the replay, the zero-product exit) have the same local
     accesses in both forms.
  2. *Partial stores.* Local memory is laid out so that consecutive 32-bit words belong to consecutive lanes (NVIDIA's documentation, and
     what makes an `LDL.128` of a warp 512 contiguous bytes): a 4-byte `STL` of a warp writes 128 contiguous bytes, four whole 32-byte
     sectors, the 8-byte `STL.64` of the old `PP` wrote eight whole sectors, and the `uint2` load of `CM` reads eight. No store of either
     form leaves a sector half written, so no read for a write.
  3. *The schedule.* The loads of x and y (`LDL.128`) sit in the same places in both forms, the distance to their use and the number of
     loads in flight per wait are the same to within one, the sum of the stall counts is 1 % lower in the new form.
  4. *`NK` 64 quads:* the same picture (1162.8 → 1163.5 instructions, 216 → 222 registers, 3067 → 3043 stall cycles per step).
  5. *The rest of the diff:* three `LDL` of the flags fewer, the stores above, and a handful of `IADD3`/`IMAD.MOV` moved; the plain and pairs
     kernels show the same (the plain kernel even issues its loads earlier: median distance to use 24 → 111 instructions).

  So nothing that the SASS shows separates the two forms by more than 1 %, and the analysis cannot name a cause of the 21 % in the kernel
  text. What it can say: whatever holds `NK` 128 quads at 125 GB/s is outside the executed instruction stream, which is the same; the
  addresses are the same too (the frame is 13,888 bytes in both forms, the stride between the regions of the warps 32 × 13,888 bytes, only
  the 8-byte `PP` slot is split into `PV` and `P2`), and the herd, the depth and the procedure are those of 1.19.2. What could not be checked
  here is the card itself: the mapping of those addresses onto the L2 slices and the memory channels of a GA107, and the compiler of the
  owner's driver (no version of `ptxas` at hand spills). A model of bytes alone was right about the bytes and wrong about the rate.
* **What is shipped.** The kernel of 1.19.2 is the default again, every module byte for byte the PTX of 1.19.2 (`KANG_META` 0 in
  `pz_kang.cl`), and the form with the tags written once is the variant "tags once" (`KANG_META` 1; `PZ_GPU_NK` with `m` after the letter,
  `64m` .. `128cm`; six more modules, 243 KB more in the executable: a module compresses to 2 to 36 KB against its sibling of the other `NK`
  but the quads modules to about 85 KB against any base, as `kang128c` already did) that the tuner measures next to the old form at every
  `NK` 64 and 128, plain, pairs and quads with CUDA and `NK` 32 to 128 plain and pairs with OpenCL, so that one `bench --variants` shows
  both forms side by side. Both forms are bit-exact against the CPU on the simulated CUDA driver and on pocl; planted keys are found with
  every new code forced on the simulated driver.
* **Prediction for the final measurement**, a model. If the 21 % is in the kernel text after all, through something the SASS does not show,
  `NK` 128 quads will measure again what 1.19.2 did, 910 to 916 M steps/s, and "`NK` 128 quads, tags once" about 716 next to it; the rule then
  keeps the default. If it is not, the two forms will measure within the noise of each other at every `NK` (the tags-once form at most +4 %
  by the bytes), `NK` 128 quads near 716 and `NK` 64 quads near 904 in both forms, and the question moves to the card: why `NK` 128, in
  every form, falls 15 to 25 % short of the bandwidth that `NK` 256 and `NK` 64 quads reach (a test: `PZ_GPU_NK=128c pzsolve bench --gpu-only`
  on a cold card against the same after `bench --variants`, and the two forms on the same herd size, which the depth of the history couples
  to `NK`).

**After 1.19.2: the hash160 of the linear kernel.** Two thirds of the instructions of a key are the hash filter `hash160_w2` (SHA-256 of the
33-byte key, RIPEMD-160 of the digest, word 2 only). This part accounts for every one of them against the floor of the expression on this
ISA, then measures the forms that move part of the hash onto the FMA pipe. Tools: NVRTC 12.0 and `ptxas` 12.9 for sm_75, sm_80, sm_86,
sm_89 and sm_90 (the counts of the 1.19.1 baseline above are reproduced exactly), `nvdisasm` 12.0, the issue model of
`tools/research/sass_sched.py` extended with an `IMAD.HI` at 1, 2 or 2.4 issue slots (the probes of `bench` measured 2.4 on the A2000) and an
`IMAD.WIDE` at 1 or 2, two staggered starts of the warps per figure (they differ by up to 1 %); clang 18 for the AMD targets. No card: every
speed below is the model, every count is SASS. Correctness: `make test`, the self-test and the planted-key hunts of `tests/e2e.sh` on the
simulated CUDA driver and on pocl, which run the plain code (the new forms are PTX inline assembly, read on the SASS).

* **Where the 2393 instructions of `hash160_w2` go** (sm_86; sm_75, sm_80 and sm_89 have the same counts, sm_90 within 3), and the floor of
  each class in the expression of the two hashes as written, with one `SHF` per rotation or shift, one `LOP3` per three-input logic function,
  one `LEA.HI` per rotate-and-add and one `IMAD` per addition:

  | class                      | count | floor | what                                                                                        |
  |----------------------------|-------|-------|---------------------------------------------------------------------------------------------|
  | `SHF.L.W` (rotations)      | 701   | 703   | SHA rounds 6 × 64 − 6 (round 0: the state is the IV), schedule 2 × (41 σ0 + 46 σ1) of variable words, RIPEMD 151 (153 steps, two rotated words dead) |
  | `SHF.R` (shifts)           | 87    | 87    | the shift of each σ0 and σ1 of a variable word                                               |
  | `LOP3`                     | 492   | 494   | Σ1, Ch, Σ0, Maj of rounds 1 to 63 (Ch and Maj of round 1 take two, their other inputs being two constants), the xor of 87 sigmas, f of 153 steps |
  | `LEA.HI`                   | 154   | 154   | rotate-and-add of 153 steps, word 0 of the message                                           |
  | `PRMT`                     | 8     | 8     | the byte swaps of the digest                                                                 |
  | other ALU                  | 11    | 9     | 7 funnel shifts of the message words (the prefix byte misaligns x), 2 `LEA` (words 0 and 8), 2 `IADD3` (round 0, the stack address of x) |
  | **ALU pipe**               | **1453** | **1455** |                                                                                           |
  | `IMAD`                     | 931   | ≈ 957 | SHA 7 per round and 3 per schedule word less the folds of the constant words, 8 digest sums; RIPEMD 2 per step, 64 word + K sums, 2 output sums |
  | loads, return              | 9     |       | x through the stack (8 `LDL`)                                                                |

  The compiler is at the floor (two short of it on the ALU pipe: it folds two of the constant logic operations the count keeps). The seven
  RIPEMD-160 steps that word 2 does not need (the last three of the left line, the last four of the right) are dropped, as the count of
  `LEA.HI` and of rotations shows; word 2 is the cheapest of the five (words 0, 1, 3 and 4 need 158, 155, 156 and 159 steps). The message
  words cannot be read from the limbs of x in any other order: the one-byte prefix shifts every word by eight bits, one funnel shift each. The
  schedule words that depend on nothing (σ1 of the length word, the zero words) are folded; the first rounds have no constant left to fold,
  since word 0 varies with x and with the parity. There is no two-rotation instruction on this ISA (`SHF` takes one source for a rotation,
  `PRMT` only moves bytes, `LOP3` has three inputs), so the ALU count of the hash cannot fall without putting shifts or rotations on the FMA
  pipe: a right shift is `mul.hi` by a power of two (one `IMAD.HI`, 2.4 issue slots by the probes), a rotation by n the high half of x · 2^n
  plus its low half (an `IMAD.HI` or `IMAD.WIDE` and an `IMAD`), a left shift one `IMAD`. The multipliers come from the constant bank
  (`pz_pow2` in pz_cuda.h), which neither compiler folds back into a shift; the shift of a constant word must stay a plain shift, or the
  compiler loses its fold and adds a test per word (`ISETP`).
* **What the model says of the two pipes**, per phase (`linear512m4`, 4 warps per partition, sm_86, the cycles of the model split by the
  phase the warps are in): in the hash the ALU pipe is busy **99.8 %** of the cycles and the FMA pipe 64 %; in the field phase the FMA pipe is
  the one at its limit. The four warps stay in step (an ALU-bound phase moves them all at the same rate), so the 16 % of idle issue cycles are
  the sum of the two imbalances, and the time of a pair of keys is close to 2 × (2 × 1453) cycles of hash plus the FMA slots of the field
  phase: 7572 against 7394. What follows: in the hash only the ALU count buys time, up to the point where 2A = A + F + O (2906 → 2393 per
  key, −18 % of the hash phase, if the moved work cost one slot each); an instruction that holds the FMA pipe for several slots blocks the
  in-order issue of its warp, and the warps being in step, of the others too, so the gain shrinks with the slot count faster than the pipe
  budget T = max(2A, 2F, A + F + O) says. The two phases do not overlap, which is where the rest is (below).
* **Measured in the model** (all five targets; cycles per 32 keys per partition for `linear512m4` with 4 warps, sm_86, two starts; the other
  targets within ±15 cycles of these, sm_90 20 to 30 below; per key of `hash160_w2`: n / ALU / FMA):

  | form                                                        | n / A / F           | wide 1: hi 1 / 2 / 2.4 | wide 2: hi 1 / 2 / 2.4 |
  |-------------------------------------------------------------|---------------------|------------------------|------------------------|
  | 1.19.2                                                      | 2393 / 1453 / 931   | 3800 / 3803 / 3793     | 4165 / 4181 / 4204     |
  | folds alone (words 0 and 8, round 0, Maj of round 1 as `IMAD`) | 2395 / 1450 / 936 | 3809 / – / 3804       | –                      |
  | σ shifts as `mul.hi` (88 `IMAD.HI`)                          | 2401 / 1371 / 1021  | 3700 / 3746 / 3808     | 4148 / 4218 / 4292     |
  | **h1** = σ shifts as `mul.hi` + folds                        | 2395 / 1364 / 1022  | 3698 / 3733 / 3803     | 4133 / 4211 / 4306     |
  | RIPEMD rotation by 10 as `mul.hi` + `mad.lo` (151 `IMAD.HI`) | 2547 / 1307 / 1231  | 3941 / 4312 / 4438     | 4315 / 4613 / 4688     |
  | **h2** = RIPEMD rotation as `mul.wide` + `mad.lo` (151 `IMAD.WIDE`) + folds | 2549 / 1304 / 1236 | 3957 / 3957 / 3969 | 4554 / 4544 / 4544 |
  | σ shifts + RIPEMD rotation as `mul.hi` (240 `IMAD.HI`)       | 2555 / 1226 / 1320  | 3950 / 4333 / 4560     | 4379 / 4782 / 4986     |

  `linear256` (free registers, 2 warps per partition: 162 to 168 registers, no spill): 1.19.2 4202 / 4610 (wide 1 / 2), h1 4165, 4222, 4295
  / 4604, 4665, 4729 at hi 1, 2, 2.4, h2 4430 / 4970. Registers and spills do not move with any form (128 for the capped modules; 12 bytes
  of spill on sm_75 and sm_80, 16 on sm_90, 0 on sm_86 and sm_89, all outside the loops over the keys, as in 1.19.2; the h1 module spills 8
  on sm_90). The folds take 3 instructions off the ALU pipe for 5 on the FMA pipe: within the noise of the model. The σ shifts gain 2.5 % if
  an `IMAD.HI` takes one slot, 1.8 % at two, and lose 0.3 % at the 2.4 the probes measured (as the energy model found after 1.19.1). The
  RIPEMD rotation adds an instruction per step (the rotation is one `SHF` but two FMA-pipe instructions) in a phase that is also near its
  issue limit, and its `IMAD.HI` form puts a long-latency instruction on the dependency chain of the step (the stall counts of the hash go from
  3762 to 4316 cycles): a loss at every slot count, 4 % with an `IMAD.WIDE` at one slot and 9 % at two. By the rules of this project (a form
  is kept only if it wins on every target in the model with the measured costs) none of them replaces the hash: the three modules are
  **forceable probes** of the two unknown costs, which one `bench` each on a card would settle: `PZ_GPU_HALF=786944` (h1, window 512 with the
  cap) against `262656`, and `1311232` (h2) against the same, `pzsolve bench --gpu-only` twice each in alternation. h1 at −2.5 % means an
  `IMAD.HI` costs one slot in this kernel (then the σ shifts should go in), at 0 % the 2.4 of the probes; h2 at −4 % means an `IMAD.WIDE`
  costs one slot, at −9 % two, which is the number the field phase (601 `IMAD.WIDE` per pair) and the energy model after 1.19.1 need and
  could not fix. The modules take 39 to 46 KB each compressed (their hash differs from the base in every line; `linear512m4h1` is stored
  against `linear256h1`, the cap costing less than the hash); the executable grows by 129 KB.
* **Tried and left out:** the rotations of Σ0 and Σ1 as `IMAD.WIDE` plus `IMAD` (the same 2-for-1 as the RIPEMD rotation, in the same
  issue-bound phase: not built after the RIPEMD result); x passed to the hash as a 16-byte aligned object (two `LDL.128` instead of eight
  `LDL`: 6 issue slots per key in a phase bound by the ALU pipe, nothing in the model); the round constant of the final word-2 sum folded
  into the target word (one `IMAD` per key, likewise); Ch of round 1 with its two constants in one `LOP3` (needs one of them in a register:
  a move for a move). On AMD the options change nothing (the kernels compile to the same code with and without them on gfx1030, gfx1100 and
  gfx906: 4234, 4572 and 4301 instructions for the linear kernel at window 256, byte for byte those of 1.19.2; `v_alignbit_b32` is one
  instruction per rotation there and `v_mul_hi_u32` runs at a quarter rate).
* **Measured on the A2000** (`bench --variants`, three turns, medians, keys/s): window 512 with the register cap 875.0, with h1 863.6
  (**−1.3 %**), with h2 808.2 (**−7.7 %**); window 256 837.4, with h1 839.9 (+0.3 %). So an `IMAD.HI` costs what the probes of `bench`
  said (2.4 slots: the model gave −0.5 % for h1 at that cost, and the loss is a little larger because the hash phase is not the whole key),
  and an `IMAD.WIDE` in the hash about two slots (the model's −9 % at two slots against −7.7 % measured). The two variants are dropped;
  the executable is back to 20 modules. The six kernels of 1.19.2 stay, and the tuner of 1.20.0 picks 256 or 512 with the register cap,
  875 M keys/s on that card, 4.5 % over the default 256.
* **What is left, and needs the kernel rather than the hash.** The hash phase leaves the FMA pipe idle 1044 cycles per key (2906 − 1862 at
  one slot per `IMAD.WIDE`) and the field phase leaves the ALU pipe idle; the field work of a pair (1582 FMA-pipe cycles at one slot, 2784
  at two) would fit in the hash's gap if the two ran in the same instruction stream. Bound: max(ALU 5812 + 1044, FMA 3724 + 1582 or 2784)
  = 6856 cycles per pair, 3428 per 32 keys against 3786 (**−9.5 %**) with an `IMAD.WIDE` at one slot and against 4165 (**−18 %**) at two.
  It needs the hash inlined into the loop over the pairs (it is a call today, which `ptxas` does not schedule across; two copies of 2400
  instructions, 77 KB of SASS on top of the 43 of the loop, against an L0 instruction cache of a few tens of KB per partition) and the point
  of pair i + 1 computed in the basic block of the hashes of pair i, under 128 registers for the capped variants (the hash state is about 30,
  the point about 60). Not done here: it is a change of the kernel's shape, which the card judged against the model once already (the peeled
  loop of 1.19.1).

## 8. Theoretical limits: how much can still be improved?

### 8.1 The constant: the figures of the earlier layouts

ALGORITHMS.md section 8.1 gives the constant of the layout in use. Its table, when the earlier layouts were still in the program, gave for
this implementation: **schema 3 (1.7.9): 1.06 measured from 2^32 to 2^44, ≈ 1.02 on the open puzzles (model, 8.10; ≈ 1.04 in 8.12)**; schema 2 (1.7.8): ≈ 1.045
with short walks (8.8); schema 1: 1.08–1.21, mean 1.15 (measured, 1.6.0; before that 1.26), ≈ 1.09 with many walkers (model, 8.7); for
schemas 1 and 2 with long walks see the correction in 8.10.

### 8.4 Cost of a step and hardware bottlenecks (1.5.0)

The first paragraphs of this section, up to the arithmetic minimum of a step, are also in ALGORITHMS.md section 8.4; they are repeated here because the text
below refers to them. The order of magnitude for puzzle 140 that closes that section is only there.

With the algorithm fixed (constant ≈ 1.15 since 1.6.0 and exponent 1/2, ALGORITHMS.md sections 8.1–8.2), what remains is the cost of **one step** and how many
steps per second the hardware can really do.

**The arithmetic minimum.** A step is an affine addition `P + S_j`: slope `λ = (y − y_j)/(x − x_j)`, `x₃ = λ² − x − x_j`,
`y₃ = λ(x − x₃) − y`, i.e. 2M + 1S plus an inversion. With Montgomery's trick on a batch of `n` walkers the inversion costs
3M per element plus `≈ 270/n` M (a single real inversion per batch). Total ≈ **5M + 1S + 270/n M**, plus two normalizations
(choosing the jump and the `±` representative require canonical `x` and `y`). Projective coordinates do not help: to choose
the jump a canonical `x` is needed, which again takes an inversion. The author knows of no way to do fewer field operations per step.

**CPU (AVX-512 IFMA, laptop processor).** Measured: a field product costs 2.7 ns per lane (throughput, 8 lanes per
instruction), a square 2.5, a normalization 0.9. The minimum above is worth ≈ 17 ns per step for arithmetic alone, ≈ 28 ns with
normalizations and subtractions. The measured step went from 52 to **40 ns** with: fully vectorized bookkeeping (192-bit distances
with vector carries, signs, tags, counters: from 10 to 1 ns), reading the jump table with one 64-byte load per
lane and an 8×8 transpose in registers (the hardware *gathers* were slower and building the vectors element by element caused
store-forwarding stalls: −8 ns), a fused backward pass, direct negation `p − y`, 4096 walkers per thread (inversion over 512
steps per lane; beyond that it does not improve). About 12 ns above the minimum remain: loads and stores of the state (≈ 200 bytes per step in
L2) and the chain of batch products. Splitting it into two independent chains gave no measurable gain (out-of-order execution
already overlaps the latencies). **The CPU kangaroo is now limited by the IFMA unit**, i.e. by computation.

**Two vectors at a time (1.7.7).** In the second half of the step one vector is a chain of about a thousand dependent instructions (product,
square, subtractions, product, normalizations, choice of the `±` representative), longer than the out-of-order execution window
(352 entries in Tiger Lake): the next vector cannot start until this one is almost finished, and the units wait for latencies. The
`llvm-mca` model (icelake-client) gives ≈ 550 cycles per vector against ≈ 260 for compute capacity alone, and the measured step (40 ns against
≈ 28 ns of arithmetic) is consistent with this reading, but it remains **a hypothesis: the author had no IFMA CPU available to measure it on**. So there is a second
form of the step, `kang_round_avx512i2` (`KV_PAIR` in `kang_vec_impl.inc`), which executes every phase of the second half of the step for two vectors
side by side (two independent products next to each other fill the gaps), with the same calculations and the same events in the same order.
The program times the two forms at first use with **all logical CPUs busy** (with the sibling thread in the same core the gaps are
already filled and the advantage vanishes) and uses the paired one only if it wins by at least 3 %; `PZ_KANG_ROUND=avx512i` or `avx512i2` force it and
`bench` lists them. On the AVX-512 path without IFMA and on AVX2 the same idea gave nothing (9.7–10.7 against 10.2–10.6 M steps/s: the vector
registers are not enough to hold two chains and the compiler adds moves), so it does not exist there. Correctness: with the emulation of the
two IFMA instructions (`tools/ifma_emul.h`, AVX-512F) the paired step, the per-lane step and the `avx512i` engine all turn out identical to the
scalar reference; `tools/verify.sh` repeats this at every release on any CPU with AVX-512.

**GPU.** *Measurement of 1.6.0 on the reference laptop GPU: 486 M steps/s with the v1 kernel* (tuning: NK 64/128/256 gives 490/487/476 M; the
variants with prefetch are the same, those with 1024 threads per SM much worse, 300 M: the kernel is limited not by
occupancy but by the traffic of the private arrays, which with 512 threads per SM are ≈ 11 MB and live in video memory). With
≈ 400 bytes per step the calculation gives 486 M × 400 B ≈ 194 GB/s, i.e. **the whole bandwidth of the card**: the v1 kernel was
100 % memory-bound. Hence the v2 (ALGORITHMS.md section 5): per step x and y are read and written (128 B), the batch products (64 B), the tags and flags
(≈ 40 B), and 1 B of history is added: ≈ **230 bytes** (estimate by construction, not measured), so the bandwidth limit is ≈ 830 M steps/s.
Instruction count (sm_86, from `ptxas`, parts measured separately): multiplication ≈ 250, square ≈ 200, subtraction ≈ 40,
normalization ≈ 30; the step is worth ≈ 2000 instructions plus the share of the inversion, which with the power `p − 2` (≈ 55,000
instructions) was ≈ 430 per step at NK = 128 (a fifth of the total) and with the divstep (≈ 10,500) is ≈ 80 (*executed per warp it was ≈ 22,000,
≈ 170 per step at NK = 128; ≈ 14,600 in 1.14.0: section 5*). At the integer throughput observed on the
linear kernel (≈ 2.5·10¹² instructions/s) the compute limit goes from ≈ 1.0 to ≈ 1.2 G steps/s: **after v2 the kernel should
still be bandwidth-limited, but close to the crossover point**. These are estimates: they have to be confirmed with `bench` on the card.
* **Measurement of 1.7.0 on the reference laptop GPU: 810.7 M steps/s** (NK 64, paired products), ×1.67 over v1; the count by construction
  (≈ 200 bytes per step with the pairs, ≈ 830 M steps/s limit at 192 GB/s) was right to within 3 %. With the bandwidth actually
  obtainable (≈ 85 % of peak) and ≈ 2200 instructions per step (at NK = 64: 5 products ≈ 1250, square ≈ 200, subtractions ≈ 320,
  inversion ≈ 160, half a product extra ≈ 125, rest ≈ 160), the kernel runs at ≈ 1.8·10¹² integer operations per second, i.e.
  plausibly **close to both limits of the card** (integer peak ≈ 20 SM × 64 × clock ≈ 1.5–2.2·10¹² depending on the clock
  under load; the linear kernel, ≈ 3300 instructions per key at 663 M keys/s, does ≈ 2.2·10¹²). The residual margin on this card is
  estimated at ≤ 15–20 %; to know for certain, the obtainable bandwidth and the clock under load are needed (`nvidia-smi`).
* **How to measure the two ceilings (1.7.1).** For each GPU `bench` prints the copy bandwidth (same traffic pattern as the state:
  coalesced reads and writes) and the throughput of `mad.lo`, `mad.hi` and the 3-input add on 8 independent chains per thread,
  with a multiple grid of every number of resident threads (no last partial wave). Dividing the bandwidth by ≈ 215 bytes per
  step and the `mad.lo` rate by ≈ 2200 instructions gives the ceilings of the model; the measured kangaroo, divided by the lower one, says
  how much margin is left. For the reference laptop GPU a copy bandwidth of ≈ 85–90 % of peak is expected (≈ 165–175 GB/s out of 192) and a
  `mad.lo` close to 64 per SM per cycle at the real clock: if the kangaroo is already at 90 % of the lower of the two, the next step is no
  longer to "remove traffic" or "remove instructions" on their own, but to do both together (a kernel with the herd in shared memory would remove
  traffic while adding a few instructions for the cooperative inversion in the block).
* **Why keeping the herd in shared memory or in L2 does not pay on this card (1.7.1).** The idea (RCKangaroo style): keep the state
  of the walkers on chip and remove the memory traffic (the ≈ 2200 instructions per step would remain, ceiling ≈ 1 G steps/s).
  Capacity: each walker takes ≈ 130 bytes (x, y, products, tag, flags); shared memory is ≈ 100 KiB per SM, i.e. ≈ 800
  walkers per SM, ≈ 1.5 per thread with 512 resident threads; the L2 of a GA107 is 2 MiB, ≈ 15,000 walkers in all
  (the current herd has 850,000 and 85 MiB). For the batch inversion to amortize, it wants at least ≈ 48 walkers per
  inversion (at 10,500 instructions each); with batches across threads the inversion, which is a serial chain of divsteps, would be run by a
  single thread per block: relative cost ≈ 153/W (W = walkers of the block), i.e. ≈ 60 % with W = 256 and ≈ 15 % with W = 1000, against
  ≈ 7 % today. In short: the traffic reduction exists only if the herd fits in the cache, and to fit in it the herd can be only a few thousand
  walkers per SM, too few to hide the latencies and to amortize the inversion. It works on GPUs with a very large L2
  (RTX 4090: 72 MiB, RTX 4070: 36 MiB), where a herd of a few hundred thousand walkers fits in L2; on a card with 2 MiB it
  does not. `bench` now prints the L2 and its ratio to the herd to check this on each card. From 1.7.1, therefore, tuning tries, only where the L2
  is at least 8 MiB, a reduced herd that fits in half of the L2 (fewer work-items, NK 16/32/64); shared memory is not useful for this
  (it is smaller than the L2 on all cards) and a kernel with cooperative inversion in the block remains unimplemented. The minimum traffic of a step in streaming is
  x and y read and written (128 bytes) plus products and metadata: ≈ 200 bytes, of which only ≈ 4 (the `prev2` copy written at every step) can
  be eliminated, and not safely (the early-exit path needs the value of the last step).
* *Batch in the warp with shuffles* (keep the walkers in registers and do Montgomery's trick across the 32 threads of a warp):
  rejected on paper. In SIMT the batch inversion is executed by all 32 threads, so each thread amortizes it only over the
  few walkers it keeps in registers (2–4): ≈ 76 multiplications per step against ≈ 11. The per-thread batch in private memory is the right
  choice for GPUs.
* *More compact state*: done in v2 (32-bit tags, a single last DP per walker, distance outside the hot loop).
* *Batch products only every 2 or 4 walkers* (the intermediate ones are recomputed from the denominators in the block): with blocks of 2
  32 B out of ≈ 230 are removed at the price of 0.5 multiplications per step (≈ +6 % of instructions) and the kernel stays at 128 registers
  without spills: **implemented as a variant** ("paired products", `PZ_GPU_NK=128b`) and left to tuning. With blocks of 4
  (−48 B, +0.75 multiplications) `ptxas` spills 344 bytes: discarded. Without a card to measure on, it is not known whether it gains.
  *The bytes of a step counted from the SASS, and what the 911 M steps/s of the A2000 mean (the quads kernel at 181 bytes per step runs at
  the copy bandwidth of the card, not above it): section 5, after 1.19.2.*

**BSGS.** After the vectorization of the windows (ALGORITHMS.md section 9.2) a giant step costs ≈ 25 ns of arithmetic and ≈ 55 ns of random
read of the table (single thread, 64 MB table). For large tables the RAM latency decides: huge pages (TLB) and
prefetch matter; a filter in cache does not help when the table itself is tens of GB, and sorting the probes by
address would give locality only with batches comparable to the table. BSGS in any case does not apply to the open puzzles (ALGORITHMS.md 8.1).

### 8.5 Comparison with RCKangaroo: what followed for this program (1.6.0)

ALGORITHMS.md section 8.5 has the calculations on puzzle 135 and the comparison as it stands. Its list, as written for 1.6.0:

1. **Constant**: 1.26 up to 1.5.0 against 1.15 for RCKangaroo. Having studied their method and remodeled it in the simulator, the
   difference was in the arrangement of the walkers; adopted (section 4.2): now ≈ 1.1–1.2.
2. **Fruitless cycles** (section 4.3): RCKangaroo handles them explicitly (cycles of 2 with a second table of jumps, longer cycles
   reconstructed in a separate kernel). Here the handling of cycles of 4 and more was missing, and with large `dp_bits` the
   walkers got stuck: fixed in 1.6.0. **It was the most serious defect**: without the fix a campaign on puzzle 140 would
   never have found the key.
3. **Step speed on GPU** (text written for v1; the v2 of 1.7.0 takes up its main points): this is the largest gap. RCKangaroo does ≈ 14.5 G steps/s on a 4090 (128 SM at 2.5 GHz). In
   proportion to integer compute capacity, on the reference laptop GPU (20 SM, ≈ 1.4 GHz) it would be ≈ 1–1.3 G steps/s; the v1 kernel did
   0.21 (puzzle 140) and 0.49 G/s measured after 1.6.0. The main differences in their kernel: a state of 24 walkers per thread kept in the L2 cache
   (on a 4090, 72 MB), no 192-bit distance in the loop (only the jump index is written, 2 bytes, and the distances are
   reconstructed in a second kernel), a fast inversion (a binary-GCD type algorithm instead of the power `p − 2`) and
   64-bit arithmetic in PTX assembly. ≈ 70–230 bytes per step, almost all in L2, against ≈ 330–400 bytes here in video memory.
   The reference laptop GPU has only 2 MB of L2, so on this card the possible gain is closer to 3× than to 5×.

### 8.6 Modular multiplication: what was tried (1.7.0)

Question: are there 256-bit modular multiplication algorithms more efficient than the one in use? Static measurements (SASS instructions
for sm_86 produced by `ptxas`, including ≈ 30 load/store instructions of the test bench), comparison at equal
function:

| operation | 8 limbs of 32 bits (in use) | 9 limbs of 29 bits, column sums in 64 bits |
|---|---|---|
| multiplication | 288 | 296 (368 forcing `mad.wide` with asm) |
| square | 232 | 256 (296) |
| subtraction | 72 | 80 |
| canonical normalization | 64 | 168 |

* **29-bit limbs with 64-bit accumulation** (the representation of the AVX2 CPU path, `fev.h`, ported to the GPU): not worthwhile. The
  count indicates that the column sum does not reduce to one `IMAD.WIDE` per product (in 1.7.0 there was no disassembler to look at the
  generated code); forcing it with `mad.wide.u32` makes the code worse. The 32-bit product
  with a carry chain already costs ≈ 3.9 instructions per limb product. Discarded.
* **One-level Karatsuba: tried (1.7.7).** 3 products 4×4 instead of 1 of 8×8 (48 limb products instead of 64), in both forms
  (additive, with 129-bit sums fixed up with masks; subtractive, with `|a1 − a0|·|b1 − b0|` and a sign), checked by program
  with 2 million products (`tools/research/sass/kara_check.c`) and counted with NVRTC 12.0, `ptxas` and `nvdisasm` for sm_86
  (`tools/research/sass_count.py`, kernels in `tools/research/sass/`). The test bench costs 96 load and store instructions, subtracted from the figures:

  | 8×8-limb product, without reduction | instructions |
  |---|---|
  | schoolbook (in use up to 1.13.0) | 184 |
  | additive Karatsuba | 248 (+35 %) |
  | subtractive Karatsuba | 256 (+39 %) |
  | by columns, one `IMAD.WIDE` and two sums per product | 232 (+26 %) |
  | by rows, one carry chain per row | 176 (−4 %) |
  | two accumulators on `mad.wide` chains (in 1.14.0, section 5) | 92 (−50 %) |

  The code generated for the schoolbook product has 64 `IMAD.WIDE.U32` (plus 24 that compute addresses in the test bench), 86 sums
  `IADD3`/`IADD3.X` and 13 `IMAD.X`: ≈ 2.9 instructions per limb product. Karatsuba saves 16 products (≈ 48 instructions) but the sums
  of the half-operands, the carry corrections, the two subtractions of `z0` and `z2` and the recomposition cost more than they remove. **It loses.**
  The row-wise product (a single `IADD3.X` per product, with the limb of the previous row as a 64-bit addend) removes 4 % of the bare product
  but does not change the complete multiplication with reduction (328 instructions, identical): 58 `MOV` remain that zero the high half
  of the addend, because `IMAD.WIDE.U32` wants a register pair. The same scheme in the square gives −15 % (232 instead of 256 instructions
  of the bench, 136 against 160 net), i.e. ≈ −1 % of the step (there is only one square per step): below the 2 % required, not adopted.
  **CPU with IFMA**: Karatsuba does not fit even in principle, because the multipliers read only 52 bits per operand and the sum of two
  52-bit limbs has 53; with 51-bit limbs the 256 bits do not fit in 5 limbs (6 would be needed: 36 products instead of 25).
* **Number of multiplications per step**: 5 products and 1 square (2 for the batch inverse, 1 for the slope, 1 for `y`, 1 for
  the running product; the square for `x`). The author knows of no form that needs fewer for an affine step with batch inversion:
  the `y` coordinate is needed to choose the `±` representative and for the next step.
* **Inversion**: here there was a gain, and it is realized (divstep, ≈ 5× fewer instructions than exponentiation).
* **Asymptotic results.** Recent preprints on integer multiplication below `n log n` (conditional and asymptotic: the constructions are
  astronomically large and no practical speedup is claimed) have no relevance for 256-bit numbers (8 limbs), where the schoolbook product wins.
  The same holds for the matrix-multiplication schemes found by automatic search (AlphaTensor, AlphaEvolve): unusable for a modular scalar product.

The only lever left on the compute side is therefore the share of multiplications in the step; the big one is memory traffic (8.4).

### 8.7 How low can it go: counting pairs with integer values (1.7.7)

ALGORITHMS.md section 8.1 gave a counting bound of `0.67·√W`, obtained by assuming that **every** pair of elements certifies two values of `x`.
Redone with more care (`tools/research/bounds.py` repeats the calculations):

* The elements are classes `{E, −E}` with `E = b·x + a` (`b = 0` tame, `b = 1` wild, `b = 2, 3, …` walkers on `2x`, `3x`, …). Two elements
  collide in two ways, `E₁ = E₂` and `E₁ = −E₂`, and give `x = (a₂ − a₁)/(b₁ − b₂)` and `x = −(a₁ + a₂)/(b₁ + b₂)`: each is an integer only with
  probability `1/|difference|`, unless the offsets `a` are chosen so that it is.
* Tame–wild: two values, always integer. Wild–wild: one, integer if the offsets are even (the "even distance" trick of the method of
  section 4.1). Tame–tame: none. With coefficients 2 or more the pairs are worth on average `1/2`, `1 + 1/3`, … and **no congruence makes
  them all integer**: a tame walker with even offsets would only see even `x`.
* With tame fraction `α` and the rest wild: coverage `c·n²/W` with `c = 2α(1 − α) + (1 − α)²/2`, maximal for `α = 1/3` with `c = 2/3`: **the
  fraction the program has used since 1.6.0** (not a chance choice: it is the exact optimum). Adding species with coefficient 2…5 the numerical optimum
  stays `(1/3, 2/3, 0, …)`; without the even-distance trick it drops to `c = 0.572` with a 5 % share of `2x` species (consistent with the
  test of 4.6: the third species cost 6 % more).
* Two ways of getting the coverage. **Structured** (sets without overlaps, as in BSGS): `E = (2/3)/√c`, i.e. **`0.8165·√W`** for
  `c = 2/3` (0.882 without the trick). **Random** (independent pairs, Poisson): `E = (√π/2)/√c`, i.e. **`1.0854·√W`** (with `c = 1/2`, tame–wild
  only, it gives `√(π/2) = 1.2533`, the value of the classic kangaroo with `±`).
* The `0.67` of ALGORITHMS.md 8.1 is the case in which every pair of different species is always worth 2, with infinitely many species: `c → 1`. With integer values that
  situation does not exist, and the optimum of the model is `0.8165`. It is not a proven lower bound for every algorithm (it remains `Ω(√W)` with unknown
  constant), but it is the best that this family of constructions (tame and wild `±1`, also with more species) can give.

**Where we are.** `tools/research/ksim.c` (abstract model, `rck=1 tf=0.3333 jabs=12`, `W = 2^34`, 1500 trials per point, error ≈ ±0.02; the cost of
placing the walkers is not counted, being negligible for `K ≪ √W`) measures the mean cost in units of `√W` as the walkers `K` and the bits of the distinguished points vary:

| walkers `K` | `dp = 0` (every point is distinguished) | `dp = 2` | `dp = 4` |
|---|---|---|---|
| 64 | 1.166 | 1.229 | 1.248 |
| 256 | 1.129 | 1.148 | 1.194 |
| 1024 | 1.108 | 1.150 | 1.248 |
| 4096 | 1.098 | 1.198 | 1.452 |

With `dp = 0` the cost goes down toward **1.085 as `K` grows**: the ideal random case is indeed the limit of this method. The overhead of the
distinguished points is ≈ `1–2 · K·2^dp / √W` (a collision that has happened is recognized only at the next distinguished point, and meanwhile all `K` walkers
advance by `2^dp` steps). At the scales of the open puzzles the ratio is tiny: `K ≈ 2^20`, `dp ≈ 40` (the table must fit in RAM), `√W = 2^69.5`
give `2^−9.5` ≈ 0.1 % (*when memory sets `dp` it is several percent: 8.9*). **The expected cost at real scales is therefore ≈ 1.09, not 1.15** (*corrected in 8.10: this holds only with short walks
or with a great many walkers; with one machine on puzzle 135 and jumps of `12·√W` the walks stay local and the cost rises to ≈ 1.35*): the 1.15 of 4.5 comes from
measurements with 64–2048 walkers on small intervals, where the overhead and the length of the walks weigh more. The rest of the gap to 1.085 comes from
the non-uniform overlap of the zones (by Jensen's inequality a non-uniform coverage costs more). Compared with the structured coverage (0.8165)
33 % is still missing, which cannot be recovered without memory `∝ √W`.

**Why structured coverage is not available.** It requires keeping the elements in memory to compare them with the new ones, i.e. memory
proportional to `√W`: for `W = 2^139` that is `2^69` entries. With the memory there is, one can only follow random walks and record distinguished
points, so the coverage is random and the value to beat is 1.085. Where memory suffices (`W ≲ 2^70`) there is BSGS (ALGORITHMS.md section 9): table
first, giant steps afterwards, cost `1.0·√W` with `√W/2` entries. New calculation: **interleaving** the two phases, also keeping the giant points, brings
the cost to `0.9428·√W` (in the pair model with `±`) but only with total memory `M ≳ 1.41·√W`; with `M = √W` it is 0.958, with `M ≈ 0.7·√W` it is worse than plain
BSGS (1.046 against 1.000). The maximum gain is therefore 5.7 %, provided there is more than double the memory of the optimal BSGS;
not implemented: the `bsgs` mode is unusable for the open puzzles anyway.

**What remains.** Between 1.15 and 1.085 there is at most 6 % to recover with random walks (fewer useless merges, better distinguished points).
The statement "no congruence makes all the pairs integer" holds pair by pair, but it does not prevent choosing *for which* `x` each
pair is integer: section 8.8 does this and goes down to 1.045 (schema 2, from 1.7.8). The absolute limit for any generic algorithm is `Ω(√W)`.

### 8.8 Below 1.085: species with fixed residues (schema 2, from 1.7.8)

The species and their table are in ALGORITHMS.md section 8.8; this is the rest of the section.

* **Walk model** (`tools/research/ksim.c design=tools/research/kang_schema2.txt jabs=12`, jumps in `[2/3, 4/3]` of `12·√W` rounded to multiples of
  12; `W = 2^36`, `dp = 0`, 5000 trials per point, error ≈ ±0.008):

| walkers `K` | schema 1 (1.6.0–1.7.7) | schema 2 | difference |
|---|---|---|---|
| 256 | 1.127 | 1.091 | −3.2 % |
| 1024 | 1.127 | 1.053 | −6.6 % |
| 4096 | 1.092 | **1.044** | −4.4 % |

  With many walkers and a negligible delay of the distinguished points (the regime of the open puzzles, 8.7, *but see the correction in 8.10*) schema 2 reaches its
  theoretical value 1.045 just as schema 1 reached its 1.085–1.09: **about 4 % fewer steps**. With few walkers and dense distinguished points
  the advantage shrinks (`K = 256`, `dp = 2`, `W = 2^32`: 1.127 for schema 2 against 1.161 for schema 1) and with 64 walkers it reverses (`W = 2^28`, `dp = 2`: 1.276
  against 1.214): with few walkers per species the coverage of each residue of `x` is too irregular. The development builds used schema 2
  only with at least 256 walkers (always, on the open puzzles); 1.9.1 uses schema 3 for every campaign, which with few walkers costs more than
  schema 1 did (kstat, 1 thread: W = 2^24 1.54 against 1.40; W = 2^28 about 1.2 for both, with a longer tail) and the same from 2^32.
* **On the real curve** (`pzsolve kstat`, 1 thread, hence `K ≈ √W/256` and automatic `dp`; measured with the development builds, where an
  environment variable chose the schema):

| W | walkers | trials | schema 1 | schema 2 |
|---|---|---|---|---|
| 2^28 | 64 | 4000 | 1.222 | 1.307 (worse: below 256 walkers schema 1 is used) |
| 2^32 | 256 | 3000 | 1.151 | 1.134 |
| 2^36 | 1024 | 2000 | 1.170 | 1.095 |
| 2^40 | 4096 | 600 | 1.146 | 1.099 |
| 2^44 | 16384 (4 threads) | 400 | 1.188 | 1.071 |

  Standard error ≈ 0.01 for the rows with 2000–4000 trials, 0.02–0.03 for the others. From `2^32` to `2^44` the mean falls from **1.164 to 1.100·√W**
  (−5.5 %), consistent with the model; the program reproduces the model even in the details (`W = 2^32`, 256 walkers, `dp = 2`: 1.134 on the
  curve, 1.127 in the model). The advantage grows with the walkers; at the scales of the open puzzles the expected value is that of the model, ≈ 1.045 (*with short walks: see 8.10*).

* **Compatibility.** The points of two schemas never meet, so each schema had its own log format (schema 2: `"PZDP3"`, base = start, fingerprint
  with `"PZDPLOG3"`). Since 1.9.1 only the format of schema 3 is read.

**What remains.** Between 1.045 and the structured coverage 0.8165 there is still 22 %, which without memory `∝ √W` does not seem recoverable with
random walks; the floor of the residue idea (0.991, without geometry losses) indicates that at most 5 % would be left to gain from better zones.

### 8.9 More RAM or a precalculation: what can be bought (analysis, after 1.7.8)

Question: with a more complex algorithm, more RAM or a precomputation kept in memory, can the factor in front of √W be lowered by a lot?
Short answer: **for a single key, no**. Memory lowers the factor only when it is of the order of √W, and a precomputation makes the
later search fast only by moving the cost, increased, into the precomputation itself.

**1. The limit for one key.** In the generic model every algorithm, with any amount of memory, must compute `Ω(√W)` elements
(Shoup, Nechaev; ALGORITHMS.md 8.1–8.2). Memory can change only the constant:

| method | mean factor | memory |
|---|---|---|
| kangaroo, schema 3 (8.10) | ≈ 1.04 (model, open puzzles, 8.12) | the table of distinguished points: negligible |
| BSGS with `±`, `T = √W/2` | 1.0 | `√W/2` entries |
| interleaved BSGS (8.7) | 0.943 | `≳ 1.41·√W` entries |
| structured coverage, tame and wild `±1` (8.7) | 0.8165 | `∝ √W` |
| pair counting (ALGORITHMS.md 8.1) | ≥ 0.67 | any |

Even with unlimited memory the maximum known gain is therefore about 10 to 20 %, and it requires `√W` entries. For puzzle 135 that is `2^66` entries, i.e.
`≈ 2·10^21` bytes at 32 bytes per entry. With memory `M ≪ √W` the gain is of the order of `M/√W` (the author's reasoning: a table of `M` fixed points
adds `n·M` useful pairs to the `n²` of the walks). With 64 GiB, `M ≈ 2^31` and `n ≈ 2^67`: `2^−36`, i.e. nothing.

**2. Precomputation (Bernstein–Lange).** Many "tame" walks that do not depend on the key are started, each up to a distinguished point, and a table
keeps the `T` distinguished points reached by the largest number of walks. Then, for a key, it suffices to walk from
`Q + r·G` to a distinguished point and hope that it is in the table. The table is valid for **every** interval of the same width (just translate the
public key). Measurements in the abstract model (`tools/research/blsim.c`, kangaroo without `±` classes, 200–400 keys per row):

| W | T | precomputation walks | precomputation | online search | product |
|---|---|---|---|---|---|
| 2^32 | 2^10 | `T`, short | 0.48·√(W·T) | 5.33·√(W/T) | 2.57·W |
| 2^36 | 2^12 | `T`, short | 0.49·√(W·T) | 5.19·√(W/T) | 2.57·W |
| 2^40 | 2^14 | `T`, short | 0.50·√(W·T) | 4.89·√(W/T) | 2.46·W |
| 2^32 | 2^10 | `16·T` | 16.5·√(W·T) | 2.13·√(W/T) | 35·W |
| 2^40 | 2^14 | `16·T` | 16.0·√(W·T) | 2.07·√(W/T) | 33·W |

  (Bernstein–Lange, with a more careful choice of walks, report 1.21·√(W·T) and 1.93·√(W/T): product 2.34·W.) The scaling laws are
  those of the theory: the search becomes `√T` times faster, but the precomputation costs `√T` times more than a whole kangaroo, and **precomputation ×
  search ≥ ≈ 2.3·W** at every size. For a single key the minimum total is `2·√(2.3·W) ≈ 3.0·√W`, three times the kangaroo (1.04–1.06).
  With `±` classes (`sym=1`) the measured product is worse (≈ 4.5·W) because the walks are strolls without drift; the author did not optimize it
  further, the conclusion does not change. The generic theoretical limit for algorithms with precomputation (Corrigan-Gibbs–Kogan) is `S·T_online² ≳ W` with
  `S` bits of table, and the known precomputations all cost `≈ √(W·S)`.

**What it would mean for puzzle 135** (W = 2^134, ≈ 1.03 G steps/s for a laptop with a discrete GPU, 32 bytes per entry):

| strategy | memory | precomputation | search |
|---|---|---|---|
| kangaroo, schema 2 | negligible | none | **≈ 4,700 years** |
| Bernstein–Lange precomputation | 16 GiB (`T = 2^29`) | 1.3·10^8 years (5·10^7 with short walks) | 138 days (358) |
| Bernstein–Lange precomputation | 64 GiB (`T = 2^31`) | 2.6·10^8 years (1·10^8) | 69 days (179) |
| BSGS | 64 GiB | table in a few minutes | 7.8·10^13 years |

The precomputation costs 10,000 to 50,000 times the kangaroo. A table prepared by others would change nothing: somebody would have had to pay for it.

**3. When precomputation pays off.** With `m` keys of the same width (or smaller) sharing a table, the total is
`P + m·S` with `P·S ≈ 2.3·W`. The minimum is `2·√(2.3·m·W)`, i.e. `≈ 3.0·√(W/m)` per key, and it beats the kangaroo (≈ 1.04·√W per key) from **about 9 keys on**.
The open puzzles with a public key were 6 when this was written (135, 140, …, 160; 135 has since been solved), with widths from `2^134` to `2^159`. The smallest dominates the cost and the others
do not help it. The same holds for multiple targets without a table (ALGORITHMS.md 8.2): with widths so different the gain is negligible.

**4. Other ways of using RAM to go faster.**
* *Cheaper step*: every new point requires at least one addition on the curve. Tables help only the starting points (the comb of
  `ec_mul_g`, already in use) and the sharing of the inversion (already done over 4096 walkers per CPU thread and over those of a GPU work-item, section 8.4).
* *More memory for distinguished points*: it lowers `dp_bits` and so the delay with which a collision is recognized, about 1 to 2
  `K·2^dp_bits/√W` (8.7). When memory sets `dp_bits` that delay is several percent: in the model of 1.12.0 (CHANGELOG), puzzle 140 with 2^22
  kangaroos costs 1.10·√W at 32 GB and 1.15 at 16 GB, against ≈ 1.04 without the delay (8.12). Entries of 16 bytes instead of 32 (the table keeps
  the index of the record in `dp.bin` and reads the distance back on a match of two kinds, after 1.12.0) have the effect of twice the RAM:
  1.08 at 32 GB, 1.10 at 16 GB, 1.15 instead of 1.23 at 8 GB. Entries of 8 bytes (the tag alone, with a scan of `dp.bin` on every match of two
  kinds) would give 1.07, 1.08 and 1.10, for a scan of a file of tens of GB, under the table lock or in the background, at every false match:
  not done. More RAM buys that back, and no more: the factor of the walk itself does not move. (The ≈ 0.1 % of 8.7 assumed `dp ≈ 40` with `K ≈ 2^20` and the RAM for that table.)
* *"Free" points*: with a single inversion one obtains both `P + J` and `P − J`, as in the giant steps of BSGS (ALGORITHMS.md section 9). In a walk the
  second point lies on no track: it is useful only if it is itself a distinguished point, i.e. once in `2^dp_bits`. Not worthwhile.

**Conclusion.** With the resources of one computer (or of many) the factor that matters is that of the kangaroo, ≈ 1.04 at large scale with schema 3 (8.12). Beyond the delay
of the distinguished points RAM does not lower it appreciably, and precomputation raises it. The levers left are those of the step (speed per step, sections 8.4–8.6) and, for a few
percentage points, the geometry of the species (8.8).

### 8.10 Local walks, long jumps and banded zones (schema 3, from 1.7.9)

The definition of `P` and the reason for the long jumps of schema 3 are in ALGORITHMS.md section 8.10; this is the rest of the section.

**Measurements.** Simulator (`tools/research/ksim.c`, which now counts merges and restarts; `rsmin=N` restarts a walker at the first distinguished point
after `N` steps) and real curve (`PZ_TEST_KTHREAD` and `PZ_TEST_JUMP` impose walkers per thread and mean jump, for tests only):

| case | `P` | cost |
|---|---|---|
| real curve, `W = 2^40`, 64 walkers, `dp = 7`, schema 2, jump `1·√W` | 0.5 | **1.548** |
| the same with jump `64·√W` | 32 | 1.299 |
| model, the same two cases | 0.5 / 32 | 1.556 / 1.270 |
| model, `W = 2^40`, 256 walkers, `dp = 4`, jumps `4, 16, 64·√W` | 16 / 64 / 256 | 1.169 / 1.168 / 1.089 |
| model, 1024 walkers, jump `64·√W` | 2048 | 1.095 |
| **model close to the real case**: `W = 2^48`, 4096 walkers, `dp = 3`, current schema 1 | 0.6 | **1.344** |
| the same, current schema 2 | 0.6 | **1.380** |
| the same, schema 2 with jumps `W^¾/16` | 16384 | 1.049 |
| the same, **schema 3** | 16384 | **1.018** |

  In the last group the model's jump is scaled so that `P` has the value it has on puzzle 135 (0.009·√W for the current schemas 1 and 2, 256·√W for the
  last two rows). In that group (300 trials per row, error ≈ ±0.035) `P` and the ratio between the spacing of the distinguished points and the length of the walks are those
  of puzzle 135 on one machine: **with schemas 1 and 2 the true cost would be ≈ 1.35–1.38·√W, not 1.10–1.15**. Restarting the walkers
  helps when `P` is small (64 walkers, `dp = 7`, jump `64·√W`: from 1.270 to 1.125, 1.106 and 1.063 restarting after 4096, 1024 and 128 steps), but
  with `P` of a few hundred it is not needed: it is not in the program (on the GPU every restart is a computation on the CPU).

### 8.11 BSGS: the cells of the table and the cost of a lookup

**1.11.0: buckets of 8 cells of 8 bytes** (`PZBSGS02`: 24-bit tag and 40-bit `b`). Up to 1.10.0 the cells were one array probed linearly at
load 0.65 (4.6 cells, 1.45 lines per lookup that finds nothing, walked with unpredictable branches, which is where the time went). With buckets of
one cache line whose 8 tags are compared at once the search became 20 to 30 % faster at equal RAM (tables of 256 MiB to 3.5 GiB). Lines read
per lookup that finds nothing, measured: 1.28 at load 0.65, 1.44 at 0.70, 1.74 at 0.75, 2.3 at 0.80, 3.5 at 0.85, 7.0 at 0.90 (a table at 0.90
holds 29 % more entries than at 0.70 and is about 4 times slower to read); 0.65 to 0.70 were equivalent per key covered, 0.75 and above lost.

**After 1.13.0: cells of 4 bytes without `b`** (`PZBSGS03`). Where the time of a giant step goes (one thread, AVX-512 lanes, Xeon at 2.8 GHz
under KVM with 1 MiB of L2, CPU time of the thread, other jobs running): arithmetic alone 36 ns (the lookups left out); the lookups add
≈ 16 ns with the table in L2 (about 45 instructions and a branch on the spill that goes either way, ≈ 40 % mispredicted) and ≈ 40 to 45 ns with a table
of 768 MiB (a DRAM access takes ≈ 200 ns on this machine, and at most 10 to 12 lines are in flight per core: ≈ 16 ns per line at best,
measured with a separate test). The 40 bits of `b` are needed only when a tag matches, which happens once per ≈ 10^6 lookups with 24-bit tags.
A cell of 4 bytes keeps a 32-bit tag only; `b` is computed again on a match by a second BSGS over `[1, T]` with a table of `2·⌈√T⌉` entries of
the 8-byte format (`bsgs_recover`; 2 ms at `T = 2^34`, 25 ms at `2^40`, 6 and 48 ms when the match is a false one; one false match per
≈ 3·10^8 giant steps). The bucket holds 16 cells, so runs of full buckets are rarer: simulated lines per lookup that finds nothing, 1.08 at
0.65, 1.15 at 0.70, 1.29 at 0.75, 1.55 at 0.80, 2.14 at 0.85, 3.85 at 0.90. Time per key covered at equal RAM, measured (one process, the tables
read in turn, 15 rounds, relative to 0.75): 1.04 at 0.65, 0.99 at 0.70, 1.08 at 0.80, 1.38 at 0.85; at equal `T` a giant step at 0.70 takes 0.92
of the time at 0.75. 0.70 was kept: 5.7 bytes per baby step instead of 11.4. Measured against 1.13.0 (7 pairs of runs, one thread, CPU time per
giant step, load average 4 to 8): same `T = 2^26` (366 against 768 MiB), 0.78 of the time per giant step (0.48 to 0.84); same RAM (`2^27` baby
steps in 732 MiB against `2^26` in 768 MiB), 0.83 (0.50 to 1.06) per giant step, each covering twice as many keys: 0.42 of the time per key
covered when the RAM limits the table. Time to the key with 4 threads and a fixed `--mem` (the same random keys for both versions, means):
`2^51` keys with `--mem 0.05`, 3.53 s against 1.19 s (20 keys); `2^49` keys with `--mem 0.025`, 1.95 s against 0.77 s (8 keys); `2^47` keys with
`--mem 0.01`, 1.01 s against 0.64 s (20 keys); `2^46` keys without a limit (the optimal table in both), 0.43 s against 0.45 s (16 keys, mostly
start-up). Building the table (`2^26` baby steps, 4 threads, 7 pairs): 1.09 times the speed (1.00 to 2.0).

**Tried and left out (in 1.14.0)**, same machine, medians of paired ratios with the quartiles:
* *Lookups overlapped with the arithmetic*: the group routine called back after each step, so that the bucket of every new x-coordinate was
  prefetched at once, the next bucket one step later when the last cell was in use, and the lookup made two steps later. Slower: 1.22 to 1.29
  times the time per giant step. Prefetching alone (no lookups at all) took the 36 ns of arithmetic to 58 to 62 ns: the reads of the table
  and the arithmetic, whose working set (≈ 0.8 MiB of 1 MiB of L2) also misses L1 all the time, share the same few fill buffers, so a read
  during the arithmetic delays it about as much as it would after it. Other machines (lower DRAM latency, more fill buffers) may differ.
* *Prefetch of the next bucket in the lookup loop* (8-byte cells): two stages (the home bucket 32 lookups ahead, its last cell read 16 ahead
  and the next bucket prefetched when it is in use): 0.95 to 1.00 [0.91–1.15]; the home and the next bucket always: 0.94 [0.86–1.02];
  distance 32 instead of 16: 1.03 [0.91–1.17]. With the 4-byte cells, distance 8, 24, 32 and 48 instead of 16: 1.02, 0.98, 0.94 and 0.97,
  every quartile range across 1. Nothing beyond the noise.
* *A flag per bucket* telling whether an entry went past it (a lookup would go on only then, not whenever the bucket is full): simulated
  1.27 instead of 1.44 lines at 0.70 with 8 cells, 1.10 instead of 1.15 with 16; not done: a bit of every tag for 5 % fewer lines.

### 8.12 Where the constant of schema 3 goes, and what is left (in 1.14.0)

Tools: `tools/research/kwalk.c` runs the walk of schema 3 as the program does (groups, a band drawn at every start, the jumps, the cycle exits,
the distinguished points, the restart on a duplicate, steps counted by whole rounds) and keeps every class visited in a second table: the step
of the first collision that solves, the distinct classes visited until then (the points of the coverage model, starting points included) and
the redundant steps; `iid=1` draws independent points from the layout instead of walking. `tools/research/kcover.py` splits the exact pair
model of 8.8 into its parts. The errors are one standard error.

**From 0.67 to the cost at the scale of the puzzles.** Each line adds one constraint to the line above.

| model | cost (`√W`) | factor | source |
|---|---|---|---|
| every pair of points certifies two keys, and no pair repeats (structured, as BSGS) | 0.667 | | ALGORITHMS.md 8.1 |
| the same pairs drawn at random (birthday: not found after `n` points with probability `exp(−n²/W)`) | 0.886 | ×1.329 | `√π/2` against `2/3` |
| species 0, 1, 3 with the residues mod 12 and the shares of schema 3, every pair inside `[0, W)`: tame–tame pairs certify nothing, pairs of one coefficient one key (0.991 with the best shares, 8.8) | 0.999 | ×1.128 | `kcover.py` |
| the zones of schema 3: 5 % of the certified keys fall outside `[0, W)` (mean λ·W 0.754 against 0.791) | 1.021 | ×1.022 | `kcover.py` |
| the keys are not covered evenly (λ·W by tenths of the interval: 0.70 and 0.66 at the two ends, 0.87 in the middle; even keys 0.71, odd 0.79) | 1.034 | ×1.013 | `kcover.py`; independent points: 1.030 ± 0.006 (`kwalk iid=1`, 8000 trials) |
| walks instead of independent points | +0 from 4096 walkers | | below |
| redundant steps (merged walkers until their next distinguished point, cycles) | +0.002 | | `kwalk` |
| delay of the distinguished points | +(1.0 to 1.2)·`K·2^dp/√W` | | `kwalk`, `dp` 2 to 8 |

With `dp_bits` set by the overhead rule (`K·2^dp/√W` between 0.005 and 0.01) the delay is 0.5 to 1 %, so the expected cost at the scale of the
open puzzles is **≈ 1.04·√W** (1.042 for puzzle 140 with a CPU herd), more where memory sets `dp_bits` (puzzle 140 with 2^22 walkers: 1.044 at
64 GB, 1.052 at 32 GB, 1.067 at 16 GB, 1.099 at 8 GB, in the model of the memory behind CHANGELOG 1.13.0 with 1.036 for the walk and 1.1 for the
factor of the delay). The ≈ 1.02 of 8.10 came from 300 trials
(±0.035); the 1.015 and 1.021 of its walk model counted steps with 4096 walkers on `W = 2^36`, whose starting points (1.6 % of `√W`) are free
in the model: `kwalk` gives 1.016 steps there for 1.031 points.

**Walks instead of independent points.** Points at the solving collision, `dp = 0`, against 1.034:

| walkers `K` | `W = 2^36` | `W = 2^40` |
|---|---|---|
| 256 | 1.069 ± 0.009 (+3.4 %) | |
| 1024 | 1.050 ± 0.006 (+1.5 %) | 1.049 ± 0.013 (+1.4 %) |
| 4096 | 1.031 ± 0.008 (−0.3 %) | 1.039 ± 0.012 (`dp = 2`) |

The excess depends on `K`, not on the length of the walks: a walk explores `W/(16·√K)` (8.10), so the walkers that can certify a given key
grow as `√K` and the coverage of every key evens out. Campaigns run 2^14 (four CPU threads) to 2^22 walkers: nothing is lost there.

**The 1.06 of the test intervals** (`kstat`, one thread, `K = √W/256`, `dp = 2`, the least the program takes) is the excess of the small herds,
a delay of 1/64 of `√W` and the free starting points:

| `W`, walkers | `kwalk`: cost | steps to the collision | delay | real curve, `kstat` |
|---|---|---|---|---|
| 2^36, 1024 | 1.071 ± 0.008 (6000 trials) | 1.057 | 0.014 | 1.066 ± 0.007 (6000) |
| 2^40, 4096 | 1.051 ± 0.012 (2000) | 1.037 | 0.014 | 1.056 ± 0.013 (2000) |

**What could still be cut.** Nothing reaches 1.5 % at the scale of the puzzles, and the program is unchanged:
* *Jump length*: mean `W^¾/4` and `W^¾` against `W^¾/16` (`W = 2^36`, 1024 walkers): `kwalk` 1.062 and 1.060 against 1.071 (±0.007 each);
  real curve, the same keys and seeds, 6000 per arm: 1.064 and 1.067 against 1.066, paired differences −0.2 % ± 1.0 % and +0.1 % ± 1.0 %.
* *Jump distribution* (`dp = 0`, 1024 walkers, 4500 trials, against 1.048 ± 0.006): spread ±5 % instead of ±33 %, 1.056; eight powers of
  two of the same mean, as in Pollard's kangaroo, 1.141 (7.5 % redundant steps: the short jumps walk back over their own points); 32 jumps
  instead of 256, 1.038, only because a walker in a cycle comes back to its own distinguished point and starts again 4200 times per trial,
  fresh starts that a campaign pays with a scalar multiplication each. Teske (*On random walks for Pollard's rho method*, Math. Comp. 2001)
  finds about 20 jumps enough for a random walk.
* *Shares and bands*: 70 bands optimized on a grid of 1536 points: 1.0321 against 1.0342. With 82 narrower bands the optimum found on a grid
  of 384 points (1.024 there) costs 1.044 and 1.045 on the grids of 768 and 1536 points: finer bands only fit the grid.
* *Restarts* (Gaudry–Schost style, or after a number of steps): they help where the excess of the walks is, below 4096 walkers (8.10), and
  every start costs a scalar multiplication.
* *The overhead term of `dp_bits`*: with `0.01·√W/K` the mean delay is 0.8 %; `0.0025` would make it 0.2 % for four times the points in the
  table and in `dp.bin`.
* *Table entries of 8 bytes* (a 30-bit tag and a 34-bit index of the record in `dp.bin`, the record read back at a tag match: a few tens of
  false matches in a table of 2^31 points, no scan of the file as in 8.9): one bit less where memory sets `dp_bits`. Puzzle 140 with 2^22 walkers:
  −2.9 % at 8 GB, −1.5 % at 16 GB, −0.7 % at 32 GB; 0.7 % or less with 2^20 walkers, nothing for CPU herds. Not done: it pays only for large
  GPU herds on machines with little RAM.
* *The negation map* is used (classes `{P, −P}`); the other point `P − S_j` of a step costs 1M + 1S and lies on no walk (8.9). *The
  endomorphism*: `λ·e mod n` of an exponent of the interval is a 256-bit number, so its collisions certify no key (4.6). *A fourth species*,
  other moduli: no share in the optimum (8.10). *Tame walkers elsewhere*: a tame–wild pair certifies both its keys inside `[0, W)` only when
  the tame point is near 0; the zones of the optimizer reach `±0.08·W` and it keeps `±0.02·W`.

**Literature**: Galbraith–Ruprai (PKC 2010), Gaudry–Schost with equivalence classes, 1.361·√W; Galbraith–Pollard–Ruprai (Math. Comp. 82,
2013), the three- and four-kangaroo methods, 1.661 and 1.714 without classes; Fowler–Galbraith (arXiv:1501.07019, 2015), kangaroo methods
with five and seven herds; Zhu, Zhuang, Yi, Lv and Lin (Des. Codes Cryptogr. 87, 2019), a variant of Galbraith–Ruprai close to 1.253·√W for
large `W`; Wu–Zhuang (Des. Codes Cryptogr., 2021) the multidimensional case; Bos–Kleinjung–Lenstra (ANTS 2010) and Bernstein–Lange–Schwabe
(PKC 2011) on the negation map and its cycles; Bernstein–Lange (*Two grumpy giants and a baby*, ANTS 2012) and Galbraith–Wang–Zhang (Adv.
Math. Commun. 11, 2017) for baby-step giant-step, with memory of order `√W`. All the low-memory methods meet the birthday factor of the second
line; none has more useful pairs than the species with residues.

**The linear search** (no public key) cannot cover fewer than `W/2` keys on average. A key `k` is tested only through `hash160` of `k·G`, and
the hashes of different keys are unrelated: after `q` distinct keys of the interval a uniformly random key is found with probability `q/W` in
any order, so the mean is `(W + 1)/2` at best. The negation map gives `(n − k)·G` with the same `x` (another compressed key, another hash) and
the endomorphism gives `λk` and `λ²k` (`x` times `β`) almost for free, but for a key of a short interval these are 256-bit numbers outside it
(each lies in the interval with probability about `W/n`). The symmetry makes points cheaper (one inversion for `C ± i·G`,
ALGORITHMS.md 2.1), not keys fewer. Only a non-uniform key (nothing of the kind in the solved keys, 4.6) or Grover's algorithm (8.2) would
change `W/2`.

### 8.13 The last percent: the delay of the distinguished points, and what the layout could still give (after 1.15.1)

The owner asked for the remaining percent of 8.12, even one, in two forms: parameters that leave the walk alone (a campaign goes on as it is)
and, for new campaigns only, a fourth schema with the shares, bands and zones re-optimized. Tools as in 8.12 (`kwalk`, `kcover.py`, `kbands.py`,
`pzsolve kstat`), the errors are one standard error. Outcome: the overhead term of `dp_bits` goes from 0.01 to 0.005 (about 0.4 % of the steps
where RAM does not set `dp_bits`, for twice the table and twice `dp.bin`); the layout is not changed, because the best layout found in the
family of the banded zones is 0.2 % better than schema 3 in the exact model and about 1 ± 0.35 % in the walk model, and the curve does not
separate the two: the bar of the question is not reached.

**What a resumed campaign needs, and what is a schema.** The jump table id (`kang_jump_id`) covers the 272 jumps, which depend on the seed and
`W` only. On resume the walkers do not take up their old walks: every session draws new starts (the herd streams are seeded with the session
number, `kang_worker_new`), and the points of the earlier sessions stay useful because the new walks run on the same jump table with the same
coefficients and residues, so a new trail meets an old one as the trails of two threads do. The shares of the species and the bands decide how
densely each part of a zone is covered, not whether the recorded points keep their meaning: a build with other shares or other bands would resume
a campaign and use its `dp.bin` (checked with a `dp.bin` of 1.15.1 resumed by this version, which also lowers `dp_bits` by one: the points are
loaded and the key is found). What cannot change without a new log format are the kind codes (coefficients 0, 1, 3 and the signs), the residues
mod 12 (points of other residues are harmless but never met) and the jumps. A new layout is therefore a question of gain alone; with the numbers
below there is none worth a second table of species.

**A. The delay of the distinguished points.** After the collision that solves, the walker that arrived second on the shared trail still has to
reach a distinguished point, `2^dp` steps on average, while the whole herd walks: about `K·2^dp` steps. The overhead rule keeps `K·2^dp/√W`
between half the term and the term, so with 0.01 the delay is 0.5 to 1 % of `√W`, 0.7 % on average over the position of `√W/K` between two
powers of two. Measured, in units of `√W`:

| | `W`, walkers | `dp` 2 | `dp` 3 | `dp` 4 | difference 3 − 2 | per `K·2^dp/√W` |
|---|---|---|---|---|---|---|
| `kwalk`, delay (cost − `t_coll`), 1500 trials each | 2^36, 1024 | 0.0149 ± 0.0011 | 0.0293 ± 0.0009 | 0.0599 ± 0.0015 | 0.0144 | 0.92 to 0.96 |
| `kwalk`, delay, 1000 trials each | 2^40, 4096 | 0.0133 ± 0.0004 | 0.0278 ± 0.0009 | | 0.0145 | 0.85 to 0.93 |
| real curve, `kstat 36 6000 1 256 dp`, the same 6000 keys and seeds in both arms | 2^36, 1024 | 1.0660 ± 0.0073 | 1.0886 ± 0.0075 | | 0.0226 ± 0.0043 (paired, correlation 0.83) | 1.45 ± 0.27 |
| real curve, `kstat 40 2500 1 256 dp`, the same 2500 keys and seeds | 2^40, 4096 | 1.0589 ± 0.0116 | 1.0702 ± 0.0114 | | 0.0112 ± 0.0065 (paired, correlation 0.84) | 0.72 ± 0.42 |

The two curve measurements together give 1.16 ± 0.22 steps per `K·2^dp` (the model 0.95, 8.12 had 1.0 to 1.2 with `dp` 2 to 8): halving the
term saves 0.0036·1.16 ≈ **0.4 % of `√W`** on average (0.3 to 0.55 % by the position of `√W/K`), with a standard error of 0.08 %. The price is
twice the entries in the table and twice the records in `dp.bin`, and only where the overhead term decides; where memory decides nothing
changes. At the scale of puzzle 140 (`√W = 2^69.5`, `--mem` 40 % of the RAM, 16 bytes per entry, 32 per record):

| walkers | RAM | `dp_bits` 0.01 → 0.005 | table | `dp.bin` | delay |
|---|---|---|---|---|---|
| 2^14 (CPU herd) | any | 48 → 47 | 0.1 → 0.3 GB | 0.1 → 0.2 GB | 0.52 → 0.26 % |
| 2^20 | 16 GB | 42 (memory) | 4.3 GB | 6.3 GB | 0.52 % |
| 2^20 | 32 GB and more | 42 → 41 | 8.6 → 17 GB (8.6 at 32 GB, within the limit) | 6.3 → 12.6 GB | 0.52 → 0.26 % |
| 2^22 | up to 64 GB | 40 to 42 (memory) | 4.3 to 17 GB | 6.3 to 25 GB | 0.5 to 2.1 % |
| 2^22 | 128 GB | 40 → 39 | 34 GB | 25 → 51 GB | 0.52 → 0.26 % |

0.0025 (four times the files of 1.15.1) would save 0.6 %: not taken, `dp.bin` is what the computers of a shared campaign carry to each other
(USAGE.md 8.1). The 8-byte entries of 8.12 remain the lever for GPU herds on machines with little RAM (−1.5 % at 16 GB with 2^22 walkers).

**B. The layout.** Three claims of 8.12 were tested as a candidate schema 4.
* *The shares of the species at the optimum of the ideal model* (0.991 against 0.999, "−0.8 %"). The ideal optimum has no tame walkers of
  residues 0 and 6 and 40 % of the herd on `3Q`; with those group totals and the bands of schema 3 inside each group the layout costs
  **1.0553** instead of 1.0342 (`kcover.py` on 1536 points): +2 %. Optimizing the totals for the layout itself gives back 1.0342 with the totals of
  schema 3 (±0.01): the 0.008 between 0.991 and 0.999 is the price of the geometry, already paid at its minimum, not a gain left on the table.
* *Bands re-optimized on a fine grid.* `kbands.py` with the specification of schema 3 (bands of 0.02, 0.1 and 0.2 `W`) on 1536 and on 3072
  points, and with bands half as wide (16, 28 and 38 bands, 186 combinations with the residues) on 1536 points, every result evaluated again
  on 3072 and on 6144 points (`kcover.py`; the layout optimized on 384 points in 8.10 fitted its grid, 8.12):

| layout | optimized on | cost on its grid | on 3072 | on 6144 | pairs outside `[0, W)` (`uniform`) |
|---|---|---|---|---|---|
| schema 3 (in use) | 384 | 1.0304 | 1.0343 | 1.0343 | 1.0208 |
| the same bands | 1536 | 1.0341 | 1.0341 | 1.0341 | 1.0207 |
| the same bands | 3072 | 1.0341 | | 1.0341 | 1.0207 |
| bands half as wide | 1536 | 1.0318 | 1.0319 | 1.0319 | 1.0170 |

  The finer bands hold on the fine grids this time (the optimizer had four times the points per band of the 8.10 run) and give **−0.23 %**,
  almost all from fewer pairs certifying keys outside the interval (mean `λ·W` 0.7594 against 0.7537); the unevenness over `x` stays.
* *Zones and the placement of the tame walkers.* The optimizer of 8.10 could place tame walkers up to `±0.08·W` and wild ones beyond the
  interval (`[−1.2, 0.2]` and `[−3.4, 0.4]`) and gave those bands no share; with wider zones (tame `±0.12·W` in 12 bands, wild `[−1.4, 0.4]`
  in 18 and `[−3.6, 0.6]` in 21, optimized on 1536 points) the optimum is 1.0341 again, with nothing outside the zones of schema 3.

**The finer bands in the walk model and on the curve.** `kwalk`, `W = 2^36`, 4096 walkers, `dp = 0` (steps by whole rounds; the 4096 starts
are 1.6 % of `√W` and free): schema 3 **1.0389 ± 0.0025** (50,000 trials over three seeds: 1.0433, 1.0369, 1.0352), the finer bands
**1.0286 ± 0.0025** (50,000 over two seeds), −1.0 ± 0.35 %; with independent points instead of walks (`iid=1`, 12,000 trials each, what the
exact model describes) 1.0245 ± 0.0050 against 1.0189 ± 0.0050, −0.5 ± 0.7 %. The three estimates (exact −0.23 %, independent points
−0.5 ± 0.7 %, walks −1.0 ± 0.35 %) are compatible with one another at two standard errors; the walks may add a little to the gain of the
exact model (a walk of 66 steps with jumps of `W/2^13` smears a start over about `W/1000`, less than a band), but the puzzles run walks of
`2^49` steps and `2^20` walkers, where that smear is `W/2^14` and the exact model is the better guide. On the real curve (`pzsolve kstat 36 N 1`
and `kstat 40 N 1`, the same keys and seeds for both layouts, one thread, a build with the finer table in place of `kang_spec3.inc`):

| `W`, walkers, `dp` | trials per layout | schema 3 | finer bands | difference |
|---|---|---|---|---|
| 2^36, 1024, 2 | 60,000 | 1.067 ± 0.0024 | 1.059 ± 0.0023 | −0.8 ± 0.3 % |
| 2^40, 4096, 2 | 10,000 | 1.051 ± 0.0056 | 1.045 ± 0.0056 | −0.6 ± 0.75 % |

Together −0.8 ± 0.3 % on the test intervals (the walks of two layouts share nothing, so the same keys pair the arms only weakly and the error
is that of independent arms). The gain of the finer bands is therefore between 0.2 % (exact model, the regime of the puzzles) and 1 % (walk
model and curve on the test intervals, 2.5 standard errors from zero and 1.8 from the exact model): the bar of the question, 1 % at three
standard errors, is not reached, and seeing 0.8 % at three would take six times the trials above (a day of four cores). A second table of species in the program
would mean either a schema of its own with its own log format (campaigns of the two schemas could no longer be merged) or a silent change of
the coverage of every campaign resumed, for a gain that the exact model puts at a quarter of a percent. The walk is not changed, schema 3
stays, and the log format with it. The layout is kept as `tools/research/kang_schema3f.txt` for whoever wants to measure
it further (`kbands.py` with 16, 28 and 38 bands on 1536 points; `kspec_table.py` turns it into a species table).

**C. Walkers per thread, restarts.** Not touched: 8.12 found no excess of the walks from 4096 walkers on, every campaign runs at least 2^14, and
4096 per CPU thread is the speed optimum of the batch inversion (8.4). A walker whose distinguished point is already in the table with the same
kind is restarted; letting it walk on would add nothing (its trail is somebody else's from there), and a start costs one scalar multiplication,
about 1.6 % of `√W` for a herd of 4096 on 2^36 and nothing at the scale of the puzzles.

### 8.14 The constant, once more (after 1.19.2)

The owner asked not to dismiss the question with a no but to search for a more efficient algorithm: a pass over the literature up to 2026 and
over the methods of the puzzle community, each result put in this project's normalization (operations per `√W`, memory, whether it applies
to one target in an interval of the integers with the negation map), and every idea not yet in 8.7 to 8.13 priced in the exact pair model
(`kcover.py`), the walk model (`kwalk.c`, `ksim.c`) or a formula. The constraints are those of the campaigns: one key, `W` from 2^134 to
2^139, at most 64 GB for the table (`--mem`), 128 GB of RAM in the machine. The bar is the one of 8.13: 1 % in the exact model, and no new log
format unless the gain pays for it (what a schema change means for a resumed campaign is in 8.13 and ALGORITHMS.md 4.7). Nothing reached
it. Each figure below says what it is: a published constant, a value of a model, or a count; nothing in this section was measured on a card.
New scripts: `tools/research/ksched.py`, `kprecomp.py`, `klambda.py`, `kideal.py`, and the option `cheap=1` of `kwalk.c`.

**A. The literature.** Costs in units of `√W` for one target in an interval of width `W`; "±" says whether the method uses the negation map.
The value to beat is 1.04 (model, 8.12; 1.06 measured from 2^32 to 2^44).

| work | result as published | here: operations / `√W`, memory, one target with ± |
|---|---|---|
| J. Pollard, *Kangaroos, Monopoly and Discrete Logarithms*, J. Cryptology 13, 2000 | two kangaroos, jumps powers of two, `2·√W` | 2.0, the table of distinguished points; no ±. With ± and random pairs the same two species give `√(π/2)` = 1.2533 (8.7) |
| P. van Oorschot, M. Wiener, *Parallel collision search with cryptanalytic applications*, J. Cryptology 12, 1999 | distinguished points, `(2 + o(1))·√W`, linear speed-up over the processors | 2.0; the memory is the table of distinguished points, as here. The general lower bound is `Ω(√W)` with no constant: V. Shoup (Eurocrypt 1997) and V. Nechaev (1994) for a group, C. Schnorr, *Small generic hardcore subsets for the discrete logarithm*, Inf. Process. Lett. 2001, for a set of `m` exponents (`≈ √m` generic steps) |
| R. Montenegro, P. Tetali, *How long does it take to catch a wild kangaroo?*, STOC 2009 | the first rigorous proof of `(2 + o(1))·√W` for Pollard's method, jumps powers of any base | 2.0, proven; nothing new to use |
| P. Gaudry, É. Schost, *A low-memory parallel version of Matsuo, Chao and Tsujii's algorithm*, ANTS 2004 | tame and wild random walks on two sets, distinguished points; one dimension `≈ 2.43·√W` | 2.43; the framework that admits equivalence classes, and the one the pair model of 8.7 generalizes |
| S. Galbraith, R. Ruprai, *An improvement to the Gaudry-Schost algorithm for multidimensional DLP*, IMACC 2009 | `≈ 2.36·√W` in one dimension, and the two-dimensional case | 2.36; no ±; the multidimensional results (also Wu, Zhuang, Des. Codes Cryptogr. 2021: an asymptotically optimal variant, heuristic) concern several unknowns, which the interval problem has not (C.9) |
| S. Galbraith, R. Ruprai, *Using equivalence classes to accelerate solving the DLP in a short interval*, PKC 2010 | Gaudry-Schost on the classes `{P, −P}`: `≈ 1.36·√W` heuristic, "not quite this fast in practice" (walks leaving the region, fruitless cycles) | 1.36 with ±; the layout of 1.6.0 (schema 1) came out of the same idea with a better arrangement (8.5, 8.7) |
| S. Galbraith, J. Pollard, R. Ruprai, *Computing discrete logarithms in an interval*, Math. Comp. 82, 2013 | three kangaroos `1.818·√W`, four `1.714·√W`, Gaudry-Schost variant `1.661·√W`; no ± (the kangaroo walk does not admit it) | 1.66 to 1.82; superseded by the ± methods |
| A. Fowler, S. Galbraith, *Kangaroo methods for solving the interval DLP*, arXiv:1501.07019, 2015 | five kangaroos `1.737·√W`, seven `1.7195·√W`, "very close to the optimal kangaroo method" | 1.72; no ± |
| M. Nikolaev, *On the complexity of the DLP in an interval in a cyclic group with efficient inversion*, Prikl. Diskr. Mat. 2015 | Gaudry-Schost with ±: `(1 + ε)·√(πW/2)` | 1.2533: exactly the random tame-wild line of 8.7 |
| Zhu, Zhuang, Yi, Lv, Lin, *A variant of the Galbraith-Ruprai algorithm with improved complexity*, Des. Codes Cryptogr. 87, 2019 | `≈ 1.253·√W` for large `W` | 1.253, the same line; the species with residues of 8.8 start below it (0.999 ideal, 1.034 with the geometry) |
| J. Bos, T. Kleinjung, A. Lenstra, *On the use of the negation map in the Pollard rho method*, ANTS 2010; D. Bernstein, T. Lange, P. Schwabe, *On the correct use of the negation map*, PKC 2011 | fruitless cycles cost most of the `√2`: 1.29 measured by the first, the full `√2` with the handling of the second | the cycle handling of 4.3 (checkpoint and escape jump) is what keeps the ± gain whole here; not a question of the constant |
| J. Liu, K. Lv, *Solving the DLP in an interval using periodic iterates*, ICICS 2017; B. Qi, J. Ma, K. Lv, *Computing the interval DLP with the restricted jump method*, Fund. Inform. 2020 | iterations of the kangaroo made cheaper in `F_p^*` by reusing earlier values (the second abstract claims a factor of at least 10 per iteration); the number of jumps is not lowered | cost per step, not per `√W`; a curve addition needs both points in full and reuses nothing (8.4): not applicable |
| D. Bernstein, T. Lange, *Two grumpy giants and a baby*, ANTS 2012; S. Galbraith, P. Wang, F. Zhang, *Computing elliptic curve discrete logarithms with improved baby-step giant-step*, Adv. Math. Commun. 11, 2017 (ePrint 2015/605) | for an interval: BSGS with ± 1.0, interleaved BSGS with ± `0.943·√W`, grumpy giants with ± `≈ 0.9·√W` (conjectural) | 0.9 to 1.0 with memory `∝ √W`: 2^66 to 2^69 entries for the open puzzles. `bounds.py` finds the same 0.9428 for the interleaved form (8.7) |
| D. Bernstein, T. Lange, *Computing small discrete logarithms faster*, Indocrypt 2012 | precomputation for an interval of order `l`: table `l^⅓`, precomputation `1.21·l^⅔`, online `1.93·l^⅓` multiplications | `1.21·√(W·T)` once, `1.93·√(W/T)` per key: priced in 8.9 and again in C.3 |
| H. Corrigan-Gibbs, D. Kogan, *The discrete-logarithm problem with preprocessing*, Eurocrypt 2018 | generic bound `S·T² ≳ N` for `S` bits of advice and `T` online operations | the precomputations above are at that bound: no cheaper table exists in the generic model |
| M. Ekerå, J. Håstad, *Quantum algorithms for computing short discrete logarithms*, PQCrypto 2017 | polynomial quantum circuits for short exponents | needs a fault-tolerant quantum computer (ALGORITHMS.md 8.2). The "quantum-inspired" classical algorithms dequantize linear-algebra sampling; a generic group search has no such structure |

Nothing from 2020 to 2026 in the venues searched (IACR ePrint, arXiv, Springer, dblp) lowers the constant of a low-memory method below the
1.253 of 2015 and 2019, which is the two-species random line this program left in 1.7.8. All the published low-memory methods sit on the
birthday line of 8.12 (×1.329 over structured coverage) and none has more useful pairs than the species with residues.

**B. The puzzle community.** JeanLucPons' Kangaroo (GitHub, 2020) states `2.08·√W` without symmetry and a 125-bit limit; its two public solves
are consistent with it (puzzle 110: 2^55.55 operations for `√W` = 2^54.5, 2.07; puzzle 115: 2^58.36 for 2^57, 2.57 with the delay of the
distinguished points). RCKangaroo (RetiredCoder, GitHub and bitcointalk topic 5517607, from November 2024) gives, in `K·√W`: classic 2.10,
"3-way" 1.60, "mirror" (two groups and the symmetry) 1.30, "SOTA" 1.15 (three groups and the symmetry; v2 with two groups, the same 1.15), and
"SOTA+" 1.02 with a *cheap second point*, or 0.99 and 1.05 by what that point is charged. The evidence is the author's own measurements
("thousands of points", no trial counts, bit sizes or error published); the 1.15 was reproduced in the abstract model as 1.10 to 1.17 by the
number of walkers (`ksim rck=1`, 8.7), and schema 3 is below it because of the residues and the bands, not of the arrangement. No public
artifact was found under the name "Kangaroo-3"; the three-kangaroo methods are the 1.818 of Galbraith-Pollard-Ruprai and RCKangaroo's
"3-way" 1.60. The cheap second point is tested in C.5.

**C. The attempts.** Every figure is the exact pair model (`kcover.py`, grid of 1536 points unless stated) or the walk model (`kwalk.c`), with
one standard error where there is one; the walk model's cost counts steps, the exact model points.

* **C.1 The endomorphism, and a fourth equivalence.** `φ(x, y) = (β·x, y)` multiplies the exponent by `λ`, a cube root of 1 mod `n`, for one
  field product; the automorphism group of a curve with `j = 0` has six elements, generated by `−1` and `φ`, so there is no fourth symmetry to
  look for on secp256k1. `tools/research/klambda.py`: the lattice `{(x, λ·x mod n)}` has determinant `n` and a reduced basis of lengths 2^128.0
  and 2^128.5, so a window of width `W` on both coordinates holds about `2·W²/n` of its points: of the exponents of puzzle 135 (`W` = 2^134)
  8·10^3 have an image in `(−W, W)`, one in 2^121, and 8·10^6 for puzzle 140 (one in 2^116); the image of every other exponent is a 256-bit
  number, and a collision with it certifies no key of the interval. An interval `[b, b + W)` has the images `λ·b + ` those, a translation: the
  count does not depend on where the interval sits, so there is no sub-case of intervals around a multiple that `λ` maps near itself. The
  first `W` for which some `x < W` has an image below `W` is the shortest vector, 2^128, four orders of magnitude below the open puzzles in
  the exponent and already `√n`. The GLV decomposition of a key of 134 or 139 bits gives two parts of 125 and 126 bits: two unknowns of 2^128
  for one of 2^134. Refuted, as 4.6 and 8.12 said; now with the count.
* **C.2 Shares that follow the progress of the search.** The only information a running search gains is that the key has not been found yet,
  so an "adaptive" herd (shares re-balanced from what the run observes) can at most follow a schedule `w(n)` of the shares by the points laid
  so far, and the same-kind merges it could observe tell it only the density it already knows. `tools/research/ksched.py` optimizes a
  piecewise-constant schedule of the shares of the 70 bands in the exact pair model (the pairs between the points of all the epochs count,
  with the analytic gradient of `∫ exp(−Λ(x, n)) dn`); the static layout computed by the same integral gives 1.0343, as `kcover.py`. Eight
  epochs (seven of `0.2·√W`, then the rest): **1.0305, −0.36 %** on the grid of 768 points and on 1536; sixteen epochs of `0.1·√W`:
  **1.0304, −0.37 %** (500 iterations: the gain converges there). The schedule starts with fewer tame walkers (20 % against 28 %) and more on `3Q'` (40 %), and ends with a third of each: the
  wild-wild pairs come first, the tame-wild pairs later, the opposite of a tame herd laid in advance (C.3). The bound of the idea is the
  `uniform` line of 8.12 (1.0209, −1.3 %), which a schedule cannot reach because the pairs of the early points stay. The shares can change
  without a new log format (8.13), but a third of a percent in the model is below the bar; a schedule also moves the shares with the
  progress of a campaign, which its resumes would have to track. Model confirmed at −0.4 %, not adopted.
* **C.3 Tame herds in advance, tables of tame trails, and the precomputation again** (`tools/research/kprecomp.py`). Three questions in one.
  *One key, tame points first*: with `P·√W` tame points laid before the wild herd starts and the pair values of schema 3's species (tame-wild
  two keys, wild-wild one), the wild steps until the key are `∫ exp(−(2Pn + n²/2)) dn`, and the total `P + ` that integral is 1.176 at
  `P = 0.1`, **1.123 at 0.3** (the minimum), 1.156 at 0.5, 1.421 at 1.0, against 1.0854 for the same pair values with the herds together
  (8.7) and 1.04 for schema 3: laying the tame points first loses 4 to 8 %, because the wild-wild pairs grow as `n²` from the start only when
  the wild walkers walk from the start (C.2 found the same sign). *A baby table serving the walks*: 8.9 priced a table of `M` points at
  `M/√W`, nothing at 64 GB (2^−36). A table of `T` distinguished points of tame trails of length `L` is worth `T·L` tame points, the pairs are
  `2·T·L·n_W` as if those points were the tame herd's own, and the trail length appears only in the delay `K·L` (a wild walker on a tame
  trail walks to its end before the match is seen, while the whole herd walks): the table size sets `L` for a given number of tame points,
  as `dp_bits` does today, and buys nothing by itself. Confirmed: the formula differs from `M/√W` only in that a trail costs its `L` steps,
  which is the kangaroo itself. *The precomputation of Bernstein-Lange*, re-priced with 128 GB (table at most 64 GiB, 16 bytes per entry,
  `T` = 2^32; 2^33 at 128 GiB changes the figures by `√2`) and 2^20 wild walkers:

  | puzzle | `√W` | Bernstein-Lange: precomputation `1.21·√(W·T)` | online `1.93·√(W/T)` | keys of the width to pay | floor of any table of tame trails, `P` tame points in `√W` (trail `L`, online with the delay) |
  |---|---|---|---|---|---|
  | 135 | 2^67 | 76,000 kangaroo searches (2^83.3 steps) | `2.9·10^−5·√W` = 2^52 steps | 76,000 | `P` = 1: `L` = 2^35, online 0.42; `P` = 4: 2^37, 0.12; `P` = 16: 2^39, 0.035 |
  | 140 | 2^69.5 | 76,000 searches (2^85.8) | 2^54.5 | 76,000 | `P` = 1: `L` = 2^37.5, 0.42; `P` = 4: 2^39.5, 0.12; `P` = 16: 2^41.5, 0.035 |

  The online search would take an hour on one GPU; the precomputation 76,000 campaigns, the same ratio as in 8.9 (the product
  precomputation × search stays `≈ 2.3·W`, the generic bound of Corrigan-Gibbs and Kogan). The floor of any table of tame trails is the
  pairs model above: a table of `P·√W` tame points makes a key cost `0.42·√W` for `P` = 1, and pays from 1.6 keys **of the same width**, the
  idealization of 8.9's "9 keys" (its 2.3·W product includes the merges and the cut trails of real walks). The open puzzles have five widths
  (136 to 140, 2^135 to 2^139); a tame set near zero serves them all, so the exact question is a shared tame herd for the five, counting pairs
  deterministically (tame-wild of one target two keys, wild-wild of one target one, wild-wild of two targets nothing: one equation, two
  unknowns): separate searches 3.44·√W₁₄₀, shared 1.88 (**55 %** of the separate total, tame herd 0.86, wild herds 0.036 to 0.51). It is the
  multi-target gain of ALGORITHMS.md 8.2, and it is a gain on the total for whoever wants all five keys; the first key (136) comes after 6.2
  times its own search in wall time, since its wild walkers are 4 % of the herd while the tame herd takes 46 %. The `dp.bin` of a finished
  campaign as the tame table of the next width: the jump table depends on `W` (ALGORITHMS.md 4.1), so the trails of one puzzle cannot be
  followed in the next and only their distinguished points would count, `M/√W` again; with one jump table for all the widths (a change of
  the format and of the jump length of 8.10) the deterministic count gives the next puzzle at most about 10 % (the `P` = 0.3 line above
  for tame points already paid). Open, a format change, and a gain for the second key of a pair of puzzles, not for the first.
* **C.4 The cost of the geometry and the start densities beyond 8.13.** The exact model splits 1.034 into 0.999 (pair values), ×1.022 (pairs
  certifying keys outside `[0, W)`) and ×1.013 (unevenness over `x`, 8.12). 8.13 halved the bands (16, 28 and 38) and found −0.23 %, holding
  on finer grids. Bands halved again (32, 56 and 76; 372 combinations with the residues, optimized on 768 points with `kbands.py`): the
  optimizer (4000 steps of exponentiated gradient over a tensor of 372 × 372 × 768) had not converged after two hours of CPU on the
  shared machine of this pass and was stopped; the number is left to whoever runs it (the command of 8.13 with 32, 56 and 76 bands,
  about three hours alone on four cores). The first halving gave −0.23 % and held on finer grids, so the second is expected below that.
  The limit of a continuous start density is bounded by the `uniform` line (1.021, the pairs lost outside the interval cannot all be
  recovered: the `+` equations of the pairs `(1, 3)` and `(3, 3)` are trapezoids over `x`, 4.1 and 8.8) and by the floor 0.999. Model
  confirmed: a few tenths of a percent, the same verdict as 8.13 (a second table of species or a silent change of every resumed campaign).
* **C.5 The cheap second point.** With one inversion a step gives `P + S_j` and `P − S_j` for `1M + 1S` more (8.9: about a third of a step).
  The second point is on nobody's trail (a walker that lands on it jumps elsewhere), so it counts only when it is itself distinguished and
  goes into the table with its kind and distance. `kwalk cheap=1` does that (`W` = 2^32, 256 walkers, 2000 trials, the same keys and seeds,
  steps in `√W`): `dp` 0: 1.094 → **0.549**; `dp` 2: 1.102 → 0.851; `dp` 4: 1.190 → 1.096. The distinct points at the collision do not move
  (1.093 to 1.13): the second points are worth a full point each when distinguished and nothing otherwise, so the steps fall by the factor
  `1/(1 + 2^−dp)`, and the extra points cost `2^−dp` of them against a third of a step for each one computed. It wins in steps at `dp ≤ 1`,
  which no campaign has (`dp_bits` 35 to 48 on the open puzzles, 4.4): the gain is `2^−35`. RCKangaroo's 1.02 for "SOTA+" is not reproduced
  by this reading at its own `dp` of 14 to 32 (1.15/(1 + 2^−14) = 1.1499); whatever its code does with the point is not in the public
  description, and no reading in which a non-distinguished point on no trail certifies a key is known to the author. Refuted at the scale of
  the puzzles, confirmed as a curiosity at `dp` 0 to 2.
* **C.6 A fourth species, all residue families.** 8.10 found no share for a fourth coefficient in `{0, 1, 2, 3}` mod 60 and `{0, 1, 3, 5}`
  mod 120 and quoted 0.957 as "the theoretical calculation with four species". The calculation: with every cross pair worth its two keys and
  every wild pair of one coefficient worth one (what the residues buy when `d | M` and `d` divides the difference or the sum of the
  residues), the pair coefficient is `c = 1 − α₀² − ½·Σ α_b²` over the shares; two wild coefficients give `c` = 0.8 at (0.2, 0.4, 0.4), cost
  `(√π/2)/√0.8` = **0.9908**, the floor of 8.8; three give 42/49 = 0.857 and 0.957; infinitely many tend to the 0.886 of all pairs worth two.
  Whether a residue family lets the pairs of a fourth coefficient reach their value evenly over `x` is a finite question;
  `tools/research/kideal.py` optimizes the shares of every (coefficient, residue) pair in the ideal model (no zones, `x` mod `M`), from random
  starts, with the Fourier view that the pairs of difference `d` cover the residues of `x` in progressions of step `M/d` and can only be
  evened out by enough classes per species. `{0, 1, 3}` mod 12: 0.9908, flat over `x` (0.7999 to 0.8001), in 1 start of 6 (the others stop at
  1.051 and 1.085: the problem is not convex). `{0, 1, 2, 3}` mod 60: 0.9908 in 2 of 12 starts, the coefficient 2 at zero share in every start (the other starts stop at 1.014 to
  1.085, among them layouts with 42 % on `2Q'` and none on `3Q'`); at the optimum the best class of coefficient 2 has the gradient
  −0.907 against −0.991 for the active classes, so a trace of it raises the cost at first order. `{0, 1, 3, 5}` mod 120: 0.9908 in 5 of 8
  starts, the coefficient 5 at zero in every start, and at the optimum (one start seeded from the solution of schema 3's family) its best
  class has the gradient −0.751 against −0.991 for the active classes: a trace of it costs at first order. The conditions for the
  fourth coefficient 5 to be worth two against every other (`5 | r₀`, `5 | r₅`, `3 | r₀`, `4 | r₅ − r₁`, `6 | r₅ + r₁`, `2 | r₅ − r₃`,
  `8 | r₅ + r₃`, with `3 | r₃`, `2 | r₃ − r₁`, `4 | r₁ + r₃`) leave the tame species 8 classes mod 120 where the pair `(0, 5)` needs 24 to cover
  every residue of `x`: the pairs of the fourth coefficient cannot be both whole and even, which is why the optimizer leaves it out. And a
  coefficient 5 has a zone `5·W` wide, so its `−` equations against 1 and 3 run at 80 % and 40 % of their ideal density before the residues
  (the same trapezoid count as in C.4). Refuted in the ideal model, where it had its best chance.
* **C.7 Walks with memory.** A walker that avoided its own recent zone would save the redundant steps only: 0.002·`√W` at 4096 walkers (8.12;
  0.0024 to 0.0068 at 256 walkers and `W` = 2^32, C.5's runs), a fifth of a percent, below the delay of the distinguished points. The long
  jumps of 8.10 already make the self-overlap of a walk negligible, and a rule that depends on the walker's history would break the merging
  of two walkers on one trail (4.3). Refuted by the count.
* **C.8 Restarts and start densities in time** are C.2 and 8.12 ("restarts help below 4096 walkers"); nothing to add.
* **C.9 The multidimensional Gaudry-Schost view.** The two-species problem has one unknown; its "dimensions" are the coefficients, and the
  Gaudry-Schost analysis (the overlap of the tame and wild sets, the birthday on the overlap) is what `kspecies.py` computes exactly by the
  correlation of the zones (the `G1`, `G2` of its geometry) for every pair of species and both equations. The multidimensional results
  (Galbraith-Ruprai 2009, Wu-Zhuang 2021) concern a point with several unknown coordinates and cost more, not less, per unknown. Nothing.
* **C.10 Other species shapes.** Non-integer coefficients do not exist (the exponent of `c·Q'` is `c·x` for an integer `c`); negative ones are
  the same species seen from the other side; coefficient 2 was refused by the ideal model (C.6, 8.10) and by the measurement of 4.6.

**D. Verdict.**

| attempt | factor against 1.034 (exact model) or 1.04 | memory | status |
|---|---|---|---|
| endomorphism `λ`, a fourth symmetry (C.1) | none: one exponent in 2^116 to 2^121 has a usable image; `Aut` has six elements | | refuted, counted |
| shares that follow the progress (C.2) | −0.36 % (8 epochs), −0.37 % (16); bound −1.3 % | none | model confirmed, below the bar |
| tame herd first, one key (C.3) | +8 % at best (1.123 against 1.040) | the table | refuted |
| table of tame trails as a baby table (C.3) | `T·L` points for `T·L` steps: the kangaroo itself; `T` sets the delay only | 64 GB | confirmed 8.9 |
| Bernstein-Lange table, 64 GiB (C.3) | online 2^−15 of a search; precomputation 76,000 searches; pays from 76,000 keys of one width | 64 GB | refuted for one key, re-priced |
| one tame herd for puzzles 136 to 140 (C.3) | 55 % of the five separate totals; the first key 6.2 times later | the table | open: a gain on the total only, nobody's goal |
| tame table of a finished puzzle for the next (C.3) | ≤ 10 % on the second key, needs one jump table for all widths | `dp.bin` of the first | open, a format change |
| start densities, bands halved twice (C.4) | run not finished in the time box (8.13: −0.23 % at half width); floor −1.3 % | none | open, bounded below the bar |
| cheap second point (C.5) | `1/(1 + 2^−dp)`: 2^−35 on the puzzles, −50 % at `dp` 0 | none | refuted at scale, confirmed at `dp` 0 to 2 |
| fourth species, residue families (C.6) | 0.957 ideal if the pairs were whole and even; they cannot be: 0.9908 stands | none | refuted in the ideal model |
| walks with memory (C.7) | ≤ −0.2 % (the redundant steps) | none | refuted by the count |
| multidimensional view (C.9), other coefficients (C.10) | nothing | | refuted |
| literature 2000 to 2026 (A) | best low-memory constant 1.253; 0.9 to 1.0 with `∝ √W` memory (2^66 entries) | | nothing below 1.04 under the constraints |

Recommendation: **nothing to implement**. No attempt reaches 1 % in the exact model under the constraints; the two that reach it in the
abstract (a shared tame herd for five puzzles, a table carried from one puzzle to the next) are gains on the total of several keys, not on
the key a campaign is after, and need a jump table common to all widths. What a future reader would need to move the constant: memory of
order `√W` (2^66 entries for puzzle 135: BSGS gives 1.0, interleaving 0.943, 8.7 and A), or a second key of the same width to share a tame
herd with (C.3), or a theorem that the random birthday line is not the floor of low-memory methods (8.12: every published method meets it;
the structured 0.8165 of 8.7 needs the memory above), or structure in secp256k1 beyond `±` (C.1: there is none that preserves an interval).
ALGORITHMS.md 8.3 points here for the best published constant.

## 10. SIMD lane arithmetic (`avx2v`, `avx512v`, `avx512i`)

The purpose of the lane engines and the representation of the field elements are in ALGORITHMS.md section 10; this is the rest of the section.

* **Reduction**: `2^261 ≡ 32·(2^32 + 977) = 0x7A20 + 256·2^29 (mod p)`: the high columns propagate into 29-bit limbs
  and fold with two small products each.
* **Limb bounds** (documented in `fev.h`, derived by hand and verified): "tight" results ≤ 2^29 + 2^23;
  "wide" operands (the sum of two tight ones) ≤ 2^30 + 2^24 are accepted by `fev_mul`/`fev_sqr`; subtraction adds a multiple of
  `p` with limbs larger than the subtrahend (`FEV_K1`, `FEV_K2`, verified by the tests to be multiples of `p`).
* **Windows and order**: round `g` covers `N` consecutive windows of 512 keys starting at `start + g·N·512` (lane `l`
  takes window `g·N + l`): after any number of rounds the covered keys are a **prefix** of the requested block, as the scheduler
  requires. Zero denominators (a center that coincides with the step) make the whole round fall back to an exact slow path,
  exercised by the tests.
* **Measurements** (x86-64 server CPU with AVX-512 and no IFMA, 4 threads): scalar `avx512` engine 23.9 M keys/s, `avx512v`
  **41.6 M keys/s (×1.74)**. Per lane, `fev_mul` costs ≈ 19 cycles (AVX-512) or ≈ 28 (AVX2) against 37 for the scalar one, `fev_sqr`
  ≈ 11 / 23 against 34; the vector unit is the bottleneck (`vpmuludq` zmm: 1 per cycle on this CPU). With AVX2 the gain
  is modest (≈ ×1.15) and the autotune picks the best engine for the CPU in use.

### 10.0 Lane kangaroo (1.5.0, `src/kang_vec_impl.inc`)

The same step as the scalar kangaroo (`kang_isa.inc`) on 4 or 8 walkers per instruction: walker `c·N + l` sits in
lane `l` of vector `c`, and all of its state stays in vector form between rounds (coordinates, 192-bit distance
as three 64-bit lanes with vector carries, sign, anti-cycle labels, counter). Only distinguished points and restarts, which are rare,
go through the single lane, and they are sorted by index as the scalar code produces them. The rules are identical (jump choice,
escape from cycles of length 2, even `y`, DP, restarts): `kang_round_check` runs the variant and the reference on the same herds
(many DPs and restarts, half of the counters beyond 2^32, one walker forced onto its own jump point, which sends the round to the scalar
fallback) and compares everything bit by bit; the selftest does it for every variant and the automatic choice excludes any that
does not match. Mutation testing: 8 errors introduced in the vector code, 8 detected.

Speed (1 thread, steps/s, same test machine): portable 4.4 M, BMI2/ADX (the version up to 1.4.0) 6.4 M, AVX2 lanes
8.3 M, AVX-512 lanes 12.7 M, **AVX-512 IFMA lanes 24.9 M (×3.9)**. With 4 threads: from 25.3 to 95 M steps/s.

### 10.1 AVX-512 IFMA variant (`avx512i`, `lib/pzcore/include/pzcore/fev52.h`)

`vpmadd52luq` / `vpmadd52huq` add to a 64-bit accumulator the low / high half (52 bits each) of the product of two 52-bit
operands. With **5 limbs of 52 bits** a full product costs 25 + 25 of these instructions, against 81 `vpmuludq` plus the
additions of the 29-bit representation. The engine (`engine_vec_impl.inc`) is the same: only the field header changes.

* **Constraint**: the instructions read only the low 52 bits of the operands, so every value that can end up in a product must
  have exact limbs. All operations return the "tight" form: limbs 0..3 < 2^52, limb 4 ≤ 2^49 (value < 2^257);
  addition also propagates the carries (there is no "wide" form).
* **Product**: 10 columns of < 2^57. The high half (weight `2^260 ≡ R = 16·(2^32+977) = 0x1000003D10`) is brought to exact limbs
  (its value is < `a·b/2^260 < 2^254`, so the top limb stays < 2^46) and folded with 10 IFMA instructions; the bits beyond
  `2^256` (limb 4 above bit 48, plus the high part of the last fold, < 2^36) are multiplied by `c = 2^32+977` with
  2 more; **a single** carry propagation follows. The square computes the 10 cross products once and doubles the
  columns (doubling an operand would exceed 52 bits): 30 IFMA instructions instead of 50.
* **Subtraction**: `4p` (or `8p` for `a − b − c`) is added, written with limbs that dominate the subtrahend (`FEV_K1`, `FEV_K2`).
* **Normalization**: a single fold leaves limb 4 ≤ 2^48, that is `r < 2^256 + 2^208`; then `r ≥ p ⇔ r + c ≥ 2^256`
  and `r − p < p` (the second fold of the 29-bit version is superfluous here: shown by the mutation test).
* **Verification**: `lib/pzcore/tests/t_fev52.c` (random values and values at the bounds of every limb, multiples of `p` ± small, long chains, inversion;
  8 mutations of the code, all detected except the one shown to be harmless), `t_engine` and `selftest` on the complete engine. Run
  both directly on the CPU of the test machine (its virtual CPUID hides IFMA but the processor executes it:
  `PZ_ASSUME_IFMA=1` skips the check, **for tests only**) and under **Intel SDE** in Tiger Lake mode, which also emulates
  CPUID. The program chooses `avx512i` only if CPUID declares IFMA, and each engine must find a known key before use
  (quick check, a few milliseconds).
* **Measurements** (same machine, 4 threads): `avx512v` 42.0, `avx512i` **64.9 M keys/s (×1.55)**; 1 thread 10.7 and 16.2.
  With IFMA the curve costs ≈ 21 ns per key and the two hashes ≈ 39 ns: **the CPU bottleneck is now the hash** (≈ 65%).
* **SHA-NI, tried and rejected**: one SHA-256 block with the SHA-NI instructions costs ≈ 63 ns on the same CPU, while SHA-256 +
  RIPEMD-160 on the 16 AVX-512 lanes cost ≈ 39 ns per key in total. It does not pay off.

### 10.2 Linear search: what was measured in 1.14.0

Same test machine as above (x86-64 server CPU with AVX-512; its CPUID hides IFMA but the processor executes it, `PZ_ASSUME_IFMA=1`), one
thread pinned to one CPU, a harness that calls the engines directly; times are the best of 100 scans of 65,536 keys, base and new run
in 9 alternating pairs (the machine was shared, load 1 to 6): median gain and range of the pairs. Instructions per key with `callgrind`
(AVX2 engines; valgrind has no AVX-512).

* **Where the time goes (avx512i).** The hash filter alone runs at ≈ 92 core cycles per key (16 lanes; about 2,700 vector ALU operations per
  batch on ports 0 and 5, so ≈ 92 % of that limit); the curve part alone (hash removed) at ≈ 18 ns per key; the whole engine ≈ 53 ns.
  The hash has no slack left: the constant message words fold, every rotation is one `vprold`, `ch` and `maj` one `vpternlogd` each.
  A field product is latency-bound when it waits for the previous one: 85 cycles in a chain against 60 when independent (`vpmadd52luq`
  runs on port 0 only); with 29-bit limbs the two are equal (throughput-bound).
* **Kept** (gain on the engine, per change):
  * canonical `x` and the parity of `y` without the full normalization (ALGORITHMS.md 2.1): avx512v +9.0 % (+6.7..+17.1), avx2v +3.7 %
    (+2.5..+5.0, 1306 to 1263 instructions per key), avx512i +2.3 % (IQR +2.2..+2.9);
  * no carry pass on the differences that only feed a product (29-bit limbs): avx512v +2.1 % (+0.6..+3.7), avx2v +1.7 % (+0.9..+2.1,
    1263 to 1237 instructions);
  * filter on digest word 2 instead of word 0: 4 RIPEMD-160 steps fewer, 5573 to 5529 instructions per AVX2 batch, 3036 to 3009 per
    AVX-512 batch; +0.3 to +0.6 % (within the noise of a single pair, consistent over all pairs);
  * one vector comparison of the filter word per batch: +0.2 to +0.35 %;
  * eight batches hashed together: avx512i +2.1 % (IQR +1.9..+3.1; 2, 8 and 16 batches gave +1.9, +2.4, +2.6 %), avx2v +0.9 %, avx512v
    unchanged;
  * two chains of products in the batch inversion (IFMA only): +3.0 % (15 pairs, +1.5..+5.0);
  * products, `vctx_emit` and `vround` inlined on AVX-512: avx512v +2.7 %, avx512i +1.3 % (IQR -0.7..+2.1: not distinguishable from 0);
  * AVX2 byte swap and rotations by 8 as one `vpshufb` with the mask in memory: 5529 to 5432 filter instructions, avx2v +0.9 %, avx2 +0.7 %;
  * scalar engines, `x(C) + x(i·G)` formed once per pair: avx2 +4.1 % (1514 to 1489 instructions), avx512 +2.8 %.
  * Together: avx512i +7.3 % (52.2 against 56.0 ns per key), avx512v +15.6 %, avx2v +7.6 %, avx2 +5.0 %, avx512 +3.3 %.
* **Tried and dropped:** a window of 512 keys on avx512i (the inversion over twice the keys): +0.15 % (-0.3..+1.2); two chains of products
  on the 29-bit layouts: -0.4 to -0.8 %; the inlining on AVX2: -1.0 to -2.4 % (16 registers, more spills); the AVX2 shuffles in the SHA-NI
  build: more spills than operations saved (1976 to 1982 instructions); several batches hashed together in the scalar engines: -0.4 % (avx2)
  and -1.1 % (avx512) against the code with one batch, 24 more instructions per key.
* **Not a bottleneck:** the claims of the scheduler last about 3 s and their set-up (the centres of the first round, two arrays of
  150 to 300 KB) costs well under 0.01 %; 4 threads give 3.95 times one thread with avx512i (`bench`: 16.7, 35.3, 66.0 M keys/s).
* **GPU linear kernel** (sm_86 SASS from NVRTC 12.0 and `ptxas`, executed path of `linear256`; not measured on a card): the filter on word 2
  with a full hash of the few keys that pass (2103 to 2077 instructions) and the shared `x(C) + x(i·G)` (loop per pair 2315 to 2269) give
  ≈ 3419 to 3370 instructions per key (-1.4 %); registers 172 to 174, same occupancy.

### 10.3 CPU kangaroo and linear search: a second round in 1.14.0

Same machine and method as 10.2 (one thread pinned, harnesses that call the step variants and the engines directly, alternating pairs,
median gain and interquartile range; the machine was shared, load 1 to 7), plus exact instruction counts by single-stepping a round
of 4096 walkers (`ptrace`; valgrind has no AVX-512) and the executed instructions sorted by kind and by source line.

* **Kept**:
  * *inversion by divsteps* (`pz_fe_inv`, the variable-time form of libsecp256k1's modinv64): 1.4 to 2 µs against 6 to 9 for the
    addition chain, about 70 chained products instead of 270. The vector inversion (`pz_fev_inv`), an addition chain of 255 vector
    squarings that wait for each other (7.3 µs with IFMA, 13 with AVX-512 and 11 with AVX2 on 29-bit limbs), now inverts the lanes
    together by Montgomery's trick with one scalar inversion: about 2 µs. Linear search avx512i +4.9 % (IQR +3.5..+6.1), avx512v +3.4 %,
    avx2v +2.8 %, avx2 +4.6 %, avx512 +5.9 %, baseline +1.8 %; kangaroo step avx512i +6.2 %, avx512i2 +6.9 %, avx2v +6.5 %, avx512v
    +1.3 % (IQR −2.0..+2.0), x86 +0.4 %; BSGS groups +3 to +5 %; the start of a herd of 4096 walkers 61 to 32 ms. The AVX2 products of
    the kangaroo step stay out of line: once the chain was gone the compiler inlined them and the step took 6.5 % more time;
  * *the stale flags of a vector tested at once*: every round looked at one byte per walker, about 12 instructions each, to find the
    restarted ones. Instructions per step: avx512i 165.4 to 152.6, avx512i2 179.7 to 166.9, avx512v 369.1 to 357.6, avx2v 827.3 to
    816.0; time +4.6 %, +3.1 %, +4.7 %, +1.6 %;
  * *a walker start with one inversion* (`u·G` in Jacobian form, `Q` added there): 32 to 27 ms per 4096 walkers, the same points;
  * *the column sums of a 29-bit product in their order*: GCC reassociated them, computed all 81 products first and spilled them
    (379 stores per vector of the avx512v step, 284 of them products). An empty `asm` after every addition (`pz_vl_keep`) keeps each
    product next to its sum: avx2v 816 to 711 instructions per step, kangaroo +10.5 % (IQR +9.9..+11.8), linear search +13.5 %, BSGS
    +5.8 %; avx512v 357.6 to 325.4 instructions (176 stores per vector), kangaroo unchanged, linear search +1.3 %.
  * Together (one thread): kangaroo avx512i +9.8 %, avx512i2 +8.8 %, avx512v +3.4 %, avx2v +12.1 %; linear search avx512i +4.9 %,
    avx512v +5.0 %, avx2v +17.0 %, avx2 +4.5 %, avx512 +5.9 %, baseline +2.8 %; BSGS groups x86 +4.1 %, avx2v +13.0 %, avx512v +5.8 %,
    avx512i +4.0 %.
* **Where the time of a step goes now** (avx512i2, executed instructions per vector of 8 walkers, 1335 in all): 357 multiply-adds
  (5 products and a square, `vpmadd52luq`/`huq`), about 470 other vector operations on the two ports that run 512-bit operations
  (carries, masks, subtractions, the transposes of the jump table), 98 stores, 82 loads and 167 register moves (the multiply-add
  overwrites its accumulator). The forward and backward passes are near the limit of those two ports; the rest is latency.
* **Tried and dropped**:
  * herds of 2048 or 8192 walkers per thread now that the inversion is cheap: avx512i2 −2.1 % and −1.5 %, avx512v +1.0 % and −1.2 %,
    avx2v +1.5 % and 0.0 % (IQR across 0 except the losses of avx512i2): 4096 stays;
  * windows of 256 keys in the lane linear engines (`VHALF` 128): avx512i +0.7 to +2.8 % in three series, lower quartile −0.3 to +0.3 %;
    avx512v −0.1 %, avx2v −0.4 %;
  * the 29-bit product row by row (17 accumulators, each limb of `b` kept in a register): kangaroo avx512v −3.7 %, avx2v −4.4 %, linear
    search −0.9 % and −1.3 %;
  * GCC's pressure-aware scheduling (`-fschedule-insns -fsched-pressure`): on the whole build, before the barrier, kangaroo avx2v +10 %,
    avx512v +5 %, but linear search avx512i −1.7 %, avx2 −1.6 %, baseline −1.7 %; on the 29-bit lane objects alone, after it, losses
    everywhere (linear search avx2v −12 %, kangaroo avx2v −6.6 %, BSGS −3 to −9 %);
  * `-fno-tree-reassoc`: on the whole build linear search avx512i −4.8 %, avx2 −2.9 %, avx512 −2.8 %; on the IFMA kangaroo objects
    alone +2.0 % (IQR +0.6..+7.0) and +1.6 % (+0.5..+2.5) with 0.8 % fewer instructions (the subtractions of `fev52.h` in another order):
    a compiler flag per object for a gain this close to the spread was not taken;
  * the AVX2 products inlined again in the kangaroo step after the barrier: 711 to 684 instructions per step, but 7.0 % more time.
* **Not a bottleneck** (estimates): a worker of the kangaroo adds its counter once per round of 4096 steps (about 120 µs); the counters
  of the workers share cache lines and the reporter reads them once a second, which costs well under 0.1 %. The linear engines add
  theirs every 8 rounds (about 1 ms). The reporter wakes every 0.1 s and takes no lock of the workers; the scheduler formats its
  checkpoint under its lock and writes it outside, and the kangaroo writes its log outside the table lock (1.11.0). Huge pages for the
  herd: about 1 MiB per thread in five sequential streams, one TLB fill per 4 KiB of each, under 0.5 %: not tried. One NUMA node and no
  SMT on this machine: neither the placement nor the mixed layout of 1.13.0 could be measured here.
* **AVX-512 against AVX2 with every core busy**: 4 threads against 1 (the other jobs on the machine took about 2 of the 4 cores at times,
  so the ratios are below 4): kangaroo avx2v 3.78, avx512v 3.79, avx512i2 3.73; linear search avx2 3.49, avx2v 3.37, avx512 3.93, avx512v
  3.80, avx512i 4.03. The 512-bit variants lose nothing at 4 threads on this machine, and the choices made on one thread stand (at
  4 threads avx512v does 1.31 times the steps of avx2v and 1.82 times its keys). On processors whose clock drops with 512-bit
  instructions on every core the margin is smaller; the kangaroo already times the IFMA pair with every thread busy.

### 10.4 Precomputed tables and constants: a third round in 1.14.0

Same machine and method as 10.2 and 10.3 (harnesses that call the step variants, the BSGS groups and the starts directly, one thread pinned,
15 alternating pairs: median gain, interquartile range and the ratio of the best runs; the machine was shared, load 2 to 5), and exact
instruction counts by single-stepping. The question of this round: which recurring computations can be read from a table or a constant
instead, and where a table is cheaper to read than its contents are to compute (the L1 data cache is 32 KiB, the L2 1 MiB).

* **Kept**:
  * *x3 and y3 of the lane kangaroo by the fast reduction* (as the linear search since 10.2): the full reduction runs only for a vector with
    a lane the fast form cannot settle. Instructions per step: avx512i 152.6 to 146.5, avx512i2 166.9 to 162.3, avx512v 325.4 to 313.6,
    avx2v 711.1 to 676.8; time +2.0 % (avx512i, IQR +1.1..+3.7), +2.5 % (avx512i2), +2.3 % (avx512v), +2.1 % (avx2v);
  * *the jump coordinates kept as `K1 − c`*: `pz_fev_sub` adds the multiple of `p` `K1` to the minuend; with `K1 − jx` and `K1 − jy` in the
    table, `x − jx` and `y − jy` are an addition and its carries, the same limbs. Per step: avx512i 146.5 to 143.2, avx512i2 162.3 to 159.2,
    avx512v 313.6 to 308.0, avx2v 671.1 to 662.1; time +0.3 to +2.0 % (within the spread on its own, the counts are exact);
  * *AVX2 jump entries as 32-byte rows transposed 4x4* (two `vmovq`, two `vpinsrq` and a `vinserti128` per vector and word before):
    avx2v 676.8 to 671.1 instructions per step, +2.3 % (IQR +1.4..+6.0);
  * *BSGS lanes with IFMA*: a tight element has exact limbs, so the canonical `x` of a window point needs only the check of the top limb:
    62.8 to 61.2 instructions per giant step, +6.1 % (IQR +5.3..+6.2). With 29-bit limbs the fast form needs a carry ripple too: avx512v
    +0.45 % instructions, avx2v −1.8 %, times within ±2 %: not used there;
  * *walker starts converted to affine form 64 at a time* (one inversion per 64 starts, the same draws and points; a batch with a start at
    infinity is drawn again one walker at a time): 4096 walkers on one thread, 20.8 to 15.4 ms (`W = 2^40`), 51.1 to 45.5 ms (`2^130`);
  * *`k·G` with signed windows of 8 bits*: a start at the scale of the puzzles was 82 % mixed additions of the comb (`libpzcore` is compiled
    without BMI2, about 3900 instructions each), up to 64 of them with unsigned windows of 4 bits. Digits in `[−128, 128]` over 33 windows of
    128 points (264 KB instead of 61, built in 3.7 ms instead of 0.9) need at most 32, a negative digit adds the negated point. 4096 starts
    at `2^130`: 45.5 to 25.9 ms; signed windows of 6 bits (88 KB) 33.6 ms, of 4 bits 52.5 ms. Every `k·G` of the program gains the same.
  * Together, against 1.14.0 (one thread): kangaroo avx512i2 +7.2 % (IQR +0.5..+10.2, best runs +7.2 %), avx512i +4.0 %, avx512v +2.7 %
    (best runs +4.8 %), avx2v +5.3 %; BSGS groups avx512i +5.0 %; the start of 4096 walkers 2.0 times faster at `2^130` (50.7 to 25.9 ms)
    and at `2^60` (26.9 to 12.6 ms). A GPU herd of 2^22 walkers is started by the CPU threads: at `2^130` about 26 s of CPU time instead of
    52. The linear search does not change.
* **Tables evaluated and left as they are**:
  * *The jump table of the kangaroo* (272 entries): 128 bytes each with IFMA (34 KiB), 192 with 29-bit limbs (51 KiB). Between two uses of
    an entry about 70 KiB of the herd (about 1 MiB per thread, read and written once per round) pass through L1, so the entries come from L2.
    Bound on any layout that would keep them in L1: all the jumps on 16 entries (2 KiB) gives avx512i2 +0.8 % (best runs +1.8 %), avx512v
    +2.0 % (+1.7 %). Entries of 96 bytes would straddle cache lines and still not fit with the herd: not done.
  * *The window of multiples of `G` of the linear search* (`i·G`, `i ≤ 256`): 20 KiB with IFMA, 36 KiB with 29-bit limbs, read in order
    once per round (L1 and L2 with the prefetchers); the denominators and their products, 148 to 296 KiB, live in L2. The batch costs three
    products per denominator, 1.5 per key, whatever the window; what a larger window divides is one inversion of a vector (2452 instructions
    per round of 4096 keys with avx512i) and the advance of the centres (about 300): 0.25 % of the 277 instructions per key. Windows of 512
    measured +0.15 % (10.2).
  * *Where the linear search spends its instructions* (avx512i, per key): 188 in the hash filter, 65 in the step and the emission of the
    candidates, 23 in the batch inversion; avx2v: 679, 383 and 64.
  * *Constant words of SHA-256 and RIPEMD-160*: the padding and length words of the 33-byte key, the zero words of the schedule and the
    terms that depend only on them already fold at compile time, and the first message word holds the top of `x`, so no state after the
    first round depends on the parity alone. What is left: the round constants are read through a pointer the compiler cannot see through,
    so in rounds 0 to 3 (where `h` is a constant) and 15 (`W = 264`) two constants are added separately. Visible constants there: 3009 to
    3006 instructions per AVX-512 batch, 5432 to 5424 per AVX2 batch, 0.1 %: not done.
  * *BSGS windows*: the table of `(i+1)·U` is 40 KiB (IFMA) or 72 KiB, the denominators and their products 328 and 590 KiB. A backward pass
    that forms each denominator again instead of keeping it (half the work area): avx512i +3.3 %, avx512v −1.2 %, avx2v −6.4 %; windows of
    512 instead of 1024 giant steps: avx512v +6.8 %, avx512i −1.4 %, avx2v −2.6 % (arithmetic only; the window is shared by the callers'
    layout of the giant steps, so one per engine would change them too). Neither was taken.
  * *The comb in BMI2/ADX code*: compiled with those flags the start code takes half the instructions but only 14 % less time (the
    additions of a start wait for each other); after the larger windows not tried again.

### 10.5 The linear search on a laptop with AVX-512, SHA-NI and a small Ampere GPU (after 1.15.0)

The owner's machine: Core i7-11850H (Tiger Lake-H, 8 cores, 16 threads, AVX-512 with IFMA, SHA extensions, 45 W), RTX A2000 Laptop (GA106, 20
SM, 4 GB, the reference laptop GPU of sections 5 and 8.4), Intel UHD (Xe-LP, 32 EU) through OpenCL, Windows 11. The question was why the linear
search felt slower than the machine should allow. The model below was built from instruction counts, the port tables of Tiger Lake (uops.info) and
what could be measured here (the test machine of 10.2 to 10.4, one thread pinned, `PZ_ASSUME_IFMA=1`); the owner's `bench` and three hunts on
puzzle 71 then came in and are the ground truth it is checked against.

* **What the laptop measured** (1.15.0, `bench`). Linear search, keys/s on 1 / 8 / 16 threads: avx512i 30.3 / 141.3 / 157.9 M, avx512v 19.1 /
  107.5 / 115.7, avx2v 10.4 / 52.9 / 57.4, avx512 11.1 / 57.7 / 71.3, shani 7.19 / 41.9 / 52.7, avx2 7.19 / 41.1 / 49.8, baseline 3.6 / 18.6 /
  22.0. Kangaroo steps/s (1 / 8 / 16): IFMA lanes 48.0 / 236 / 261 M, IFMA pairs 52.3 / 230 / 261, AVX-512 lanes 23.5 / 132 / 140, the mixed
  layout 205 (−22 %, not used). A2000 through CUDA: linear 793 M keys/s (window 512 with the register cap), kangaroo 895 M steps/s, copy
  bandwidth 164 GB/s of 192, probes of `mad.lo` 2.42 T/s, `mad.hi` 1.00, three-input add 2.52 T/s. Intel UHD through OpenCL: 17.5 M keys/s, 56 M
  steps/s. Hunts on puzzle 71 (Speed rows after about two minutes): default (16 threads, both GPUs) 865 M keys/s = CPU x15 133 + A2000 715 + UHD
  16; `-t 8` 882 = 113 + 754 + 16 (after 9.5 minutes); `--no-gpu` 153.5. Then with the UHD excluded (`--gpu 0`): `-t 14` 948 = 142 + 807 after
  two minutes; `-t 8` 854 = 125 + 729 after 8.5 minutes (the long runs sit lower than the two-minute ones: the laptop throttles over time, so
  only runs of equal length compare).
* **Instructions per key** (single-stepped here, one round of 4096 keys): avx512i 279.9 (188 of them the hash filter: 3009 per batch of 16 keys),
  avx512v 397.5, avx2v 1237 and avx2 1489 (callgrind, 10.2). The `shani` engine (the scalar curve of avx2, SHA-256 on the SHA instructions,
  RIPEMD-160 on 8 AVX2 lanes) cannot run here: from its code the filter is about 3120 instructions per 8 keys (1150 the SHA-256 stage, 64
  `sha256rnds2` for 8 keys; 1970 RIPEMD-160), 390 per key against 679 for avx2 and 188 for avx512i, about 1200 per key with the curve.
* **The ports of Tiger Lake** (uops.info; the same as Skylake-X for 512-bit integer operations): `vpaddd` and `vpternlogd` zmm on p0 or p5 (two per
  cycle), `vprold`, `vpsrld` and `vpmadd52luq` zmm on p0 only, `vpshufb` zmm on p5 only; the second 512-bit FMA unit of the server parts plays no
  role (no floating point). Per key of avx512i about 224 vector operations go to those two ports: 169 in the hash (62 of them rotations and shifts,
  p0 only), about 55 in the curve (26 IFMA, p0 only); the two ports balance at **about 112 cycles per key, the floor**. Measured: 33.0 ns per key on
  the laptop at one thread (30.3 M keys/s), 158 cycles at the 4.8 GHz turbo (fewer if the AVX-512 licence lowers it), 50.2 ns here at about 3.2 GHz:
  **the two cores do the same work per clock, 70 to 75 % of the port floor**; the rest is latency (the field products are a chain, 10.2). The
  kangaroo step: 20.9 ns on the laptop, 27.6 ns here; one thread does 0.55 to 0.63 keys per step.
* **CPU: what the 8 cores give.** 141.3 M keys/s on 8 threads is 17.7 M per core, **2.8 GHz on every core** at 158 cycles per key: the all-core
  AVX-512 clock of a 45 W part (the single core runs at 4.8). SMT adds 11.8 % (157.9 M on 16 threads): the second thread of a core fills part of
  the 25 to 30 % the first leaves idle on p0 and p5, and the working set of a thread (window table 20 KiB, denominators and products 296 KiB, hash
  batches 12 KiB) fits twice in the 1.25 MiB L2. The hunt without GPUs gives 153.5 M, 3 % under `bench` (claims, the reporter, heat). What is left on
  the CPU is the 25 % of latency between the port floor and the engine, which the two chains of products and the eight batches of 10.2 already
  attacked; nothing in the tables of 10.4 reaches it.
* **SHA-NI on Tiger Lake, by its port cost** (uops.info): `sha256rnds2` is one uop on **p5 only, one every 3 cycles** (latency 6); `sha256msg1` 6
  uops (p01, p015, p5), one every 4 cycles; `sha256msg2` 12 uops, one every 5. A 33-byte key is one block, 32 `rnds2`, 12 `msg1`, 12 `msg2`: SHA-256
  alone costs **96 cycles of p5 plus about 108 of p0/p1 per key**, against about 55 cycles of p0 + p5 per key (27 of p5) for the 16-lane AVX-512
  SHA-256 (1750 operations per 16 keys). A hybrid engine (SHA-256 on the SHA unit, RIPEMD-160 and the curve on the lanes) would need at least 96 +
  35 (RIPEMD-160, 1120 operations per 16 keys) = 131 cycles per key for the hash against 92 now: **−30 to −35 % on the engine**, with p0 and p1
  taken by the message schedule into the bargain; computing the schedule on the lanes (17 cycles per key) leaves the 96 cycles of p5. Not
  implemented. The laptop confirms the table: `shani` measures exactly `avx2` on one thread (7.19 M both; the SHA unit replaces 290 instructions
  per key by a p5 queue of the same length), +6 % on 16 threads. On Zen 3 and Zen 4 `rnds2` issues every cycle (`msg1` 0.5, `msg2` 2): about 62 cycles
  per key against 73 for the 8-lane AVX2 SHA-256 on three ports, which is where the `shani` engine can win; `--isa auto` measures and decides.
* **GPU: the A2000 is on the ceiling of its ALU pipe.** The sm_86 SASS of `linear256` (NVRTC 12.0, `ptxas`): 2791 instructions per key, of which
  `hash160_w2` 2030 (794 `SHF`, 495 `LOP3`, 457 `IADD3`, 155 `LEA.HI`, 108 `IMAD.IADD`, 8 `PRMT`, 8 `LDL`) and the field about 700 (354 `IMAD`, mostly
  `IMAD.WIDE.U32`, 232 `IADD3.X`, 49 `SEL`, the rest shifts and control); `linear512` with the register cap, the variant the tuner chose on the card,
  is the same within a few instructions, 128 registers, four blocks of 128 per SM, one 4-byte spill outside the loop over the keys. On GA10x an SM
  partition dispatches one warp instruction per cycle; `IADD3`, `LOP3`, `SHF`, `LEA`, `SEL` and `PRMT` run on the 16-lane ALU pipe, `IMAD` on the
  16-lane FMA pipe, so a warp instruction holds its pipe two cycles. Per key about **2230 instructions go to the ALU pipe and 470 to the FMA pipe**.
  The probes of `bench` give the clock: `mad.lo` 2.42 T/s is 16 lanes × 80 partitions × **1.89 GHz** (the card boosts past its 1687 MHz rating when
  cold), and the three-input add 2.52 T/s is that pipe plus a few `IMAD.IADD`. The ALU pipe at 1.89 GHz does 2.42 T operations/s, which is **1.09 G
  keys/s at 2230 per key**; measured 793 M, **73 % of the pipe**, and 2.21 T instructions/s in all: the
  kernel is issue-bound on one pipe, and only fewer ALU-pipe instructions per key help; the FMA pipe is a fifth busy. The 27 % missing are
  dependency stalls (the hash is a chain of 4 to 5 cycle operations with 2 to 3 independent at a time; the capped variant with 16 warps per SM
  was chosen for that). Moving adds to the FMA pipe was tried on the SASS: `mad.lo` with the constant 1 is folded back to `IADD3` by `ptxas`; with
  a multiplier it cannot see, the three adds of a SHA-256 round move (192 `IMAD`) but the three-input fusion of `IADD3` is lost: ALU pipe −3.8 %,
  instructions +2.9 % per hash; all the adds of both hashes: ALU −8 %, instructions +14.5 % (the `LEA.HI` rotate-and-add fusion goes too). Not
  worth a card's time. The kangaroo is no calibration for the linear kernel: 895 M steps/s × 1132 instructions use 40 % of the ALU pipe; it sits
  between its two ceilings (760 M by bandwidth, 1.86 G by instructions, `bench`).
* **The campaign against `bench`** (puzzle 71, both GPUs, two minutes): the A2000 does 715 M with 15 CPU threads and 754 M with 8, against 793 M
  alone; the CPU 133 M on 15 threads against 148 expected from `bench` (15/16 of 157.9), and 113 M on 8 threads against 141: **CPU and GPU each
  lose 5 to 20 % to the other**, and the losses move together with the thread count: the chassis shares one power and heat budget (Dynamic
  Boost, 45 + 95 W nominal). The thread that serves the CUDA device is not the cause: it sleeps on the context (`CU_CTX_SCHED_BLOCKING_SYNC`) and
  per launch of 0.35 s (about 250 M keys) it computes three `k·G`, writes 264 bytes and reads 8, under a millisecond; and with 8 threads it had
  eight idle CPUs and the card still lost 5 %. The first-run measurement does run everything together and tries 16, 12, 8, 4 and 0 threads
  (with the high priority for the GPU threads, which it chose on this machine); it saw no configuration beyond its 3 % margin, which the numbers
  confirm: `-t 8` against the default is +2 % on the total (882 against 865). From this round it also tries one thread fewer per CUDA device, and
  it leaves a GPU out when its loss is not measurable (within 1 %, confirmed in alternating pairs): the Intel UHD gives 16 M keys/s, 1.9 % of
  the total, costs the core that drives it (the 16th thread, about 2 M) and draws from the package the CPU cores run on, the 10 to 15 M more that
  the CPU loses with the GPUs on than its 16th thread is worth. The two runs with `--gpu 0` (the index `devices` gives the A2000) then showed
  the whole size of it: **the A2000 goes from 715 to 807 M keys/s and the total from 865 to 948 M (+10 %)** once the UHD stops drawing from the
  shared budget, far more than the 16 M it contributed; the first-run measurement had tried that configuration and seen nothing beyond its 3 %
  margin, because the power moves back to the card over more than its 10 s window. The new rule leaves such a device out even when the window
  shows no gain. SMT: +12 % for the CPU alone, so 16 threads stay right; with the A2000 running the thread count is a wash within ±2 % (`-t 14`
  and `-t 8` after two minutes). Whether the thread serving the card needs a CPU of its own is still to be measured (`-t 16` against `-t 15` with
  `--gpu 0`): the first-run measurement now tries both. The driver API does not expose the clock or the throttling state of the card (NVML would,
  a new dependency): `nvidia-smi -q -d CLOCK,PERFORMANCE` during a hunt shows them.
* **Expected on this laptop:** CPU 150 to 158 M keys/s alone, 125 to 145 with the A2000 running; A2000 790 to 810 M alone or with the CPU, 715
  to 760 with the UHD also on; **together 940 to 960 M keys/s after two minutes with `--gpu 0`** (880 to 900 after ten, when the chassis is
  hot), 865 with every device. The best choice is `--gpu 0` with 14 to 16 threads; 8 threads cost about 4 % and leave the machine usable. The kangaroo line of the puzzle-140 run (GPU 486 M steps/s after 5.8 s against 895 in `bench`)
  is a warm-up reading: the launches grow from 2 rounds to the 0.35 s target by at most a factor of 2 each, the first ones are mostly launch
  overhead and the 1.3 M walkers of the herd (10240 work-items × 128, the same herd as in `bench`) and those of the CPU threads take seconds of
  CPU time to start (10.4).
  After a minute 800 to 850 M steps/s is what to expect with the CPU threads running.
* **What would move the numbers.** CPU: only the latency between the port floor (112 cycles per key) and the engine (158): a third of it is in the
  hash, whose 16 lanes are one chain of 64 + 80 rounds; two batches interleaved at the instruction level would need 32 registers for the states
  alone. GPU: fewer ALU-pipe instructions in the hash (1909 of 2230 per key); the SHA-256 message schedule is 48 × 11 operations of that pipe per
  key, the RIPEMD-160 lines 160 × 7, both at the minimum of their expressions. Neither is a table or a constant (10.4). What a laptop user can do
  today is in USAGE 8: `--gpu 0`, `-t`, `--prio-gpu`, `--limit`, `--retune`; the heat budget decides the rest.

### 10.6 Cycles per key and per step: where the latency is (after 1.15.1)

The question of this round: how much of the gap between the port floor and the measured cycles of the lane engines (10.5: about 25 %) can
be closed, with latency, not instruction counts, as the target. Same test machine, one thread pinned, alternating pairs (9 to 11, median and
interquartile range; the machine was shared, load 2 to 7), exact instruction counts by single-stepping, plus three things the earlier rounds did
not have: the core clock and the ports measured with microbenchmarks that carry their own clock reference (a chain of `imul` on port 1, which
512-bit code leaves alone, in the same loop as the block under test), the hash timed alone, and the engine with the hash removed and single parts
of the step knocked out.

* **The machine, measured.** The clock is 3.29 GHz on scalar code, 3.10 with 512-bit adds, **2.60 to 2.75 with a dense stream of IFMA or
  `vpmuludq`**, and the licence outlasts the hash phases: the IFMA engines run at about 2.65 GHz throughout, so the 50.9 ns per key of avx512i
  are **135 cycles**, not the 160 that 3.2 GHz would give (10.5 assumed 3.2 and so overstated the cycles of this machine; the laptop's clock
  under AVX-512 is lower than its 4.8 GHz turbo in the same way). The ports: on this Xeon `vpmadd52luq`/`huq` and `vpmuludq` on 512 bits issue on
  **two ports** (24 independent IFMA in 12.7 cycles, 8 IFMA beside 8 shifts in 8), shifts and rotates on port 0 only, shuffles on port 5 only,
  register moves are eliminated, and the broadcast-operand forms run at full speed. uops.info gives Tiger Lake one port for IFMA and `vpmuludq`
  (10.5), so the two machines share the hash floor but not the floor of the curve part: per 16 keys of avx512i, 440 port-0-only operations
  (336 IFMA, 104 shifts) against 304 others, 27.5 cycles per key on Tiger Lake, about 24 here with the IFMA spread over two ports.
* **The hash alone is at its floor.** 16-lane SHA-256 + RIPEMD-160 to the filter word, 3009 instructions (2663 on the two vector ports:
  958 rotates and shifts on port 0, 66 shuffles on port 5, 1639 either), port bound 1332 cycles per batch of 16 keys. Measured alone: **1361
  cycles (85 per key), the same whether the batches depend on each other or not**, and two batches interleaved round by round 1368. Inside
  the engine a pass costs 91 (measured by hashing every batch twice and three times: +34.5 ns per key and pass at 2.65 GHz). So the hash is
  port-bound at 93 to 98 % and has no latency to give; the 92 cycles and "92 % of the port bound" of 10.2 and 10.5 were this same figure
  at the wrong clock. The AVX2 hash (8 lanes, 5432 instructions, three ports): 1677 cycles per batch, 210 per key.
* **Where the 135 cycles of a key go (avx512i, before this round):** hash 91; curve part 44 (16.2 ns with the hash removed): products 16
  (6.0 ns: with the products replaced by additions 10.1 ns), batch inversion 12 (4.6 ns), and **15 in the carry passes and the emission of the
  candidates** (eight `pz_fev_sub`/`sub2` carry ripples per pair of points, 13 cycles each and on the chain of every point). A product in a
  chain costs 72 TSC ticks here against 51 when independent (`vpmadd52` latency 4: a column waits for 9 multiply-adds, then the reduction tail
  of about 29 cycles), two chains side by side already reach the independent figure (51.5), and the products of the step are 2 chains.
  The curve part thus runs at about half its port floor here; on Tiger Lake, where the IFMA have one port, the model below puts it near that floor.
* **Tools.** `llvm-mca` (LLVM 18, `-mcpu=tigerlake` and `skylake-avx512`, same model) assigns 512-bit adds to three ports and IFMA/`vpmuludq`
  to two, with a 224-entry window: it gives 394 cycles per 16 keys for the curve loop (measured about 750 here) and 39 per kangaroo step (measured
  about 75), so its absolute figures are of no use here; the relative change of a loop body is. A small simulator with the port table of uops.info
  (IFMA, shifts, rotates on port 0; shuffles on port 5; the rest on both), 4-cycle IFMA, a 97-entry scheduler and 224-entry window (160/352 for
  Tiger Lake) gives the hash at 1478 per batch (measured 1361) and the curve loop at 506 per 16 keys on the Tiger Lake ports; it is a scratch tool,
  not in the repository. The single-stepped counts and the pairs decide.
* **Kept: the subtrahends go into the products** (`pz_fev_mul_add`, `pz_fev_sqr_add`: `a·b + c` with `c = K1 − y` or `K2 − x − x'`, limbs below
  2^55, added to the low columns before the reduction, whose own carry pass makes the result tight; the constants of a round are formed once).
  The linear step loses the `sub2` after the square and the `sub` after the last product, the kangaroo step the `sub` after its last product and
  the carry pass of `y3` in `kv_canon`: two of the four carry-pass stages of each chain. Instructions: linear 279.9 to 275.2 per key (loop body
  979 to 903 per 16 keys), kangaroo lane step 143.2 to 141.1, pair step 159.3 to **161.4** (more: the two-vector pass spills the constants it
  now forms, which is why the pair does not gain). Time: **linear avx512i +2.1 % (IQR +1.9..+2.2;
  +2.0 % in a second series), the curve part alone +7.1 % (+7.0..+8.0); kangaroo avx512i +3.3 to +3.9 % (IQR +2.8..+4.0), avx512i2 within the
  spread (−0.3, +1.2 and +0.4 % in three series, IQR −0.8..+1.1)**. Models: mca (Tiger Lake) curve loop 394 to 346 cycles per 16 keys
  (−12 %), lane step 438 to 427, pair step 620 to 611; simulator on the Tiger Lake ports 506 to 464, 478 to 468, 763 to 726. The 29-bit engines
  are unchanged (their products accept
  loose factors but a lazy result cannot be subtracted from without a carry pass; the 29-bit code compiles as before, same counts). The helpers
  that form the constants are inlined on AVX-512, and the hash of the batches is kept out of the loop of the step explicitly: with the helpers
  inlined GCC had pulled `vctx_flush` into the loop body (1401 instructions with the hash call inside) and the gain fell to +1.5 %.
* **Checked on the final form.** The three IFMA objects (engine, lane and pair kangaroo) built with gcc 13 and clang 18 at `-O1`, `-O2` and
  `-O3`, each with real IFMA (`PZ_ASSUME_IFMA=1`: this CPU executes the instructions, its CPUID hides them) and with `tools/ifma_emul.h`: all
  twelve selftests pass (612 checks), with no compiler warning; AddressSanitizer with UBSan, real and emulated, report nothing; the MinGW-w64
  build passes the selftest and `t_engine` under wine with IFMA assumed (the plain `make wintest` skips the IFMA engine here). valgrind 3.22 cannot
  run AVX-512 (SIGILL on the first instruction). The helpers are correct without the forced inlining too: the form with them out of line passes
  the same selftest. One crash was seen on the way, and it was not the compiler: an object built from a half-edited source passed `&cx`, the address
  of the pointer variable, where the pointer was expected (`const pz_fev **` for `const pz_fev *`), which GCC 13 only warns about
  (`-Wincompatible-pointer-types`); the callee read 0x80 bytes past a stack slot that is 32-byte but not 64-byte aligned and `vmovdqa64` faulted.
  Putting that one mistake back into the final source reproduces the fault at the same instruction. Worth knowing for the next change in this
  code: a warning of that kind in the build log of a lane engine is a bug, and the engine files compile with it as an error in the checks above.
* **Tried and dropped** (same pairs, numbers at 2.65 GHz):
  * *the low and the high halves of the products in separate accumulators* (a column waits for 5 multiply-adds instead of 9): a product in a
    chain 72 to 61 ticks, an independent one 51 to 53 (8 additions and 10 zeroing moves more), but the loop bodies grew 17 % (linear) and 9 %
    (pairs) and the engines lost: linear −0.6 % (curve part alone −1.2 %), pairs −2.8 %, lanes +4.3 %. The chain was not what the engine
    waited for;
  * *four vectors at a time in the backward pass of the kangaroo* instead of two: −10 % (905 instructions per vector against 815, spills);
  * *two batches hashed together, round by round* (the brief's "two batch halves interleaved"): the hash alone 1368 against 1361 per batch, in the
    engine +0.8 % (IQR +0.5..+0.8, nine pairs all positive) for 44 KB of code and a second copy of the rounds; on AVX2 it is slower (2010 against
    1677, spills). Not taken: the hash is port-bound already;
  * *four chains in the forward pass of the batch inversion*: not measured, two chains already run the products at their independent throughput;
  * *the front end*: the loop bodies are 6 to 44 KB, far beyond the uop cache, and the legacy decoder feeds 10-byte instructions at 1.1 per cycle
    here; but 6-byte register forms, which these loops mostly are, still reach 2 per cycle in 24 KB blocks, and the hash alone runs at 98 % of
    its port bound from the decoder: not a limit here;
  * *the word extraction of the emission* (four 64-bit words and dword selects instead of eight masked words): 12 operations fewer per 8 keys,
    about 1 % of the loop body, left for a later round.
* **What is left.** The hash, two thirds of a key, is at its port floor: only fewer operations would help, and 10.2 found none. The curve part
  runs at about half its port floor on this machine (20 of 135 cycles per key are latency), less on Tiger Lake by the model; what would recover
  it is the curve arithmetic interleaved with the hash of the previous batch at the instruction level (the hash saturates the ports the curve
  leaves idle), which needs the hash rounds and the products in one basic block with 32 registers between them: not tried in this round.
  Expected on the laptop of 10.5 from the models: about −2 % on the linear search and −2 to −3 % on the kangaroo step.

### 10.7 The hash and the step in one instruction stream, and the ports of the hash (after 1.18.0)

The question of this round: the last lever that 10.6 left, the curve arithmetic of the next points interleaved with the hash of the
previous batch at the instruction level (software pipelining of the two phases of the lane engines), and whatever else the `avx512i`
engine still had. Same test machine (Cascade Lake Xeon, 4 cores, no SMT; its CPUID hides IFMA but the processor executes it,
`PZ_ASSUME_IFMA=1`), one thread pinned, alternating pairs of a harness that calls the engines directly (9 to 11 pairs, median and
interquartile range; the machine was shared, load 1 to 2), the hash alone timed in TSC ticks per batch (best of 3000), exact instruction
counts by single-stepping a round of 4096 keys, and `llvm-mca` (LLVM 18, `-mcpu=tigerlake`) for the relative figure of two loop bodies.

* **Kept: the hash with fewer operations on the two vector ports** (`hash_lanes.inc`, `hash_tables.h`). Two things the compiler did not
  do on its own. GCC 13 compiles the majority function of SHA-256, written as `(a & b) ^ (a & c) ^ (b & c)`, to **two** `vpternlogd` where
  one does (it fuses three-input logic only when the expression matches one of its patterns; `ch` and the RIPEMD-160 functions already
  came out as one): with 16 lanes it is now the intrinsic with immediate `0xE8`, with 8 and 4 lanes the four-operation form
  `((a ^ b) & c) ^ (a & b)`. And the round constants of RIPEMD-160 were immediates moved to a general register and broadcast with
  `vpbroadcastd`, one micro-operation on port 5, the port the shuffles need and one of the two the whole hash runs on; with 32 registers
  GCC rematerialized them 57 times per batch (56 `mov` and 57 broadcasts in the filter). They are now a table per step (the round constant
  plus the padding word a step reads when it reads one, so that nothing the compiler used to fold is lost) read through a pointer it
  cannot see through, as the SHA-256 constants were since 10.2: a broadcast operand of the addition, a load on ports 2 and 3. Per batch of
  16 keys (filter): **3009 to 2904 instructions, 550 to 489 `vpternlogd`, 57 to 30 `vpbroadcastd`, 56 to 27 scalar moves**; what is left
  are the initial state words of both hashes, which the first rounds fold on. The hash alone 1239 to 1209 TSC ticks per batch (−2.4 %);
  the engine **avx512i +2.0 % (IQR +0.8..+2.1, 11 pairs, ten of them positive), 275.2 to 268.6 instructions per key**; avx512v +1.2 %
  (IQR +0.4..+3.5). The AVX2 filter goes from 5432 to 5427 instructions per 8 keys, the SSE2 one from 7640 to 7648 (the four-operation
  `maj` in GCC's SSE2 code): both within 0.1 %, and their timings were inside the noise of the shared machine. The hash was at 98 % of
  its port floor (10.6); it still is, the floor is lower.
* **Tried and dropped: the step of the next pair interleaved with the hash of the last batch.** The prototype (kept as a patch outside the
  tree, with the notes of this round): the lane hash rewritten in stages, SHA-256 in blocks of 16 rounds and RIPEMD-160 in blocks of 20
  steps with the state in a struct the compiler keeps in registers (the plain functions are the stages in a row and compile to the same
  code within two instructions); the loop of the step hashes the batch completed two passes earlier, one block between each of its six
  products and the emission of its candidates, the ring of batches holding three. Correct (`t_engine`, the planted keys); the loop body
  goes from 937 + 3008 instructions (step, hash) to **4185**, with the stack traffic of the step from 20 stores and 65 loads to 91 and
  194 per 16 keys (the state of the hash, 24 vectors, and a product, 20, do not share 32 registers), 275.2 to 290.4 instructions per key.
  Time: **−0.5 % (IQR −1.1..+2.7, 11 pairs) and −0.2 % (9 pairs)**: no gain. `llvm-mca` for Tiger Lake agrees: the serial bodies 1486
  cycles per 16 keys, the interleaved one 1550 (+4 %). Why nothing came of it: the hash is bound by the two vector ports (98 %), so it has
  no idle slots to fill, and the step, whose latency the merge was to hide, costs its slots and its bytes in the merged stream too; what
  the merge could recover is the latency slack of the step (about 140 cycles per 16 keys here) and what it pays is 240 more instructions.
  A probe of the front end settles the other half: a 28 KB block of independent 7-byte instructions decodes at **2.31 per cycle** on this
  machine (3.95 from the micro-operation cache for the same block at 64 instructions), and the hash, the step loop and the merged body all
  run at 2.2 to 2.25 instructions per cycle: they sit on the legacy decoder's 16 bytes per cycle as much as on the ports, and a loop body
  of 27 KB does not fit any micro-operation cache (1.5 K entries here, 2.25 K on Tiger Lake). On Tiger Lake the merge is worth even less by
  the port table: IFMA and the rotations are port 0 only there, so the step is port-0-bound (440 cycles per 16 keys against 372 here) and
  the merged floor (1704) is within 4 % of the separate ones (1332 + 440). The cheaper variants of the same idea: `-funroll-loops` on the
  engine object +0.06 % (IQR −1.1..+0.5; 4026 to 3947 instructions in the scan function, the loop of the step not unrolled); `#pragma GCC
  unroll 2` on that loop is ignored by GCC 13 (object unchanged); clang 18 for the engine object **−28 %** (it keeps the hash and the
  products out of line and spills the columns). The register copies of the hash (272 per batch: a `vpternlogd` overwrites its first
  operand, and `ch`, `maj` and the RIPEMD-160 functions need all three inputs afterwards) cost nothing but bytes: they are eliminated at
  rename on Skylake-X and Cascade Lake and on Ice Lake, Tiger Lake and Rocket Lake alike (uops.info: 0 executed micro-operations); reordering
  the RIPEMD-160 step so that the rotation of `c` comes first and the function could overwrite it changed nothing (284 copies either way:
  GCC's register allocation does not follow the source order).
* **SMT on the laptop of 10.5** (16 threads give 9 % more than 8 on the i7-11850H). Nothing in the engine makes the second thread of a core
  hurt the first: the window of multiples of `G` (20 KiB with IFMA) is one static table per process, read in order as broadcast operands
  (loads on ports 2 and 3; a miss to L2 costs no slot on the ports the engine is bound by); the denominators and their products (164 KiB per
  thread) live in L2 (1.25 MiB per core on Tiger Lake, twice 164 fits); the batches are 5 KiB per thread; the comb table of `k·G` (264 KB)
  is read once per claim, for the centres. The engine on one thread is at 90 to 95 % of the floor of ports 0 and 5, so the sibling can only
  take what the step's latency leaves idle: **+9 to +12 % is what the port table predicts**, and 16 threads stay right. The test that would
  show otherwise, on the laptop: `start /affinity 0x1 pzsolve hunt 71 --no-gpu -t 1` against `start /affinity 0x3 ... -t 2` (Windows numbers
  the two threads of a core next to each other, so this runs both threads on core 0; the program's own placement is a hint the mask
  overrides) and `start /affinity 0x5 ... -t 2` (two cores), Speed rows after a minute each; on Linux `taskset -c 0,8` and `-c 0,1`. The
  second over the first is the gain of one core from SMT (expected 1.09 to 1.12; 1.0 or less would mean the sibling costs the first thread
  something, and a profile with 8 threads would be the answer), the third over the first should be 2.
* **What is left.** The lane hash has 2578 operations on the two vector ports per 16 keys and runs at 96 to 98 % of that; the compiler now
  emits one instruction per operation of the algorithm, so only a change of algorithm (none known for one block of SHA-256 and RIPEMD-160
  with 32-bit rotations) would lower it. The step is within 10 % of its port floor on Tiger Lake. The engine as a whole is close to what
  these ports allow per core; more keys per second on that machine come from the clock (10.5) and the GPU (section 5).

### 10.8 The order of the IFMA curve step, and two kinds of dead operations (after 1.19.1)

The question of this round: 10.6 found the curve part of the IFMA engines latency-bound (a product that waits for the one before it), so
can the step be scheduled by hand, intrinsics in a chosen order or inline assembly, so that independent chains of multiply-adds interleave
and the critical path shrinks, without assuming the latencies of one processor (GCC and clang, Linux, macOS and MinGW, every CPU with
AVX-512 IFMA)? The test machine of this round is another one: a Xeon of the Sapphire Rapids family (model 207, 4 virtual CPUs of a shared
host; its CPUID reports IFMA). Measured on it: `vpmadd52luq` on 512 bits issues **two per cycle** with a latency of 4 (twelve independent
accumulators 0.50 cycles each, a dependent chain 2.80 TSC ticks, so the core runs at 1.43 times the TSC under IFMA). The host was busy
(load 1 to 4, from other jobs), and the time of one round varied by ±10 to ±17 % from one round to the next: the timings below come from a
harness that links the two builds into one process and alternates them round by round (2000 to 4000 pairs, median of the per-pair gains;
an A/A control of the same harness gave −1.8 to +0.6 %), with 11 whole-process pairs as a check, exact instruction counts by
single-stepping, and `llvm-mca` (LLVM 18) on the loop bodies for `-mcpu=icelake-server`, `tigerlake`, `sapphirerapids` and `znver4`, a
change being kept only if it is not worse on any of them.

* **Where the latency is, on this machine.** A product in a chain costs 57 to 60 ticks (about 84 cycles), two chains side by side 35 to
  38 ticks per product, against a port floor of about 46 cycles (62 multiply-adds and about 30 other operations on two ports). The curve
  part of the linear search alone (hash removed) runs at 20.6 ticks per key, 29.4 cycles, and its port floor (the 696 two-port operations of
  the loop per 16 keys plus three products per denominator) is about 30: **on a core with two IFMA ports the linear search has no latency
  left to recover**, and the hash is at its floor too (10.6, 10.7); with one IFMA port (Ice Lake and Tiger Lake client; Zen 4 splits a
  512-bit operation in two halves) the step is port-bound (10.6). The kangaroo is different: its forward pass (8 to 11 ticks per step, about a fifth of the
  round) is at its port floor (155 vector operations per vector of 8 walkers, 24 of them the shuffles of the transposes), its backward
  pass runs at 67 to 76 % of its floor: that is where the latency is.
* **Kept: two kinds of dead operations in the products** (`fev52.h`). The carry pass of the high half of a product cleared the carried
  bits from limbs 5 to 8, which only go to the multiply-adds of the fold, and those read the low 52 bits of their operands: four masks per
  product and square that did nothing. And the columns of `pz_fev_mul_add` start from the limbs of the added term instead of zero, five
  additions fewer (the square doubles its columns, so `pz_fev_sqr_add` adds after). Instructions per step: lane kangaroo 141.0 to 137.0,
  pairs 161.3 to 152.6 (their products are out of line, and calls are counted); the product bodies (gcc and clang) 128 to 125 instructions,
  `llvm-mca` equal or better on all four targets (Ice Lake and Tiger Lake −4.6 %, Sapphire Rapids and Zen 4 equal); on the whole loops of
  the kangaroo the Sapphire Rapids model gives +0.7 % (backward) and +1.8 % (forward) with the same block throughput: three more register
  copies from GCC's allocation, which that model executes and the hardware eliminates (the Zen 4 model eliminates them). Measured: lanes
  **+1.8 %**, pairs **+1.3 %** (medians of 2000 pairs; whole-process pairs +1.7 % and +3.9 %).
* **Kept: the `x` of a candidate as four 64-bit words** (`vctx_emit`, IFMA only, as 10.6 had proposed): each 64-bit word of `x` is split into
  its 32-bit halves by one `vpermd`, the low halves of the eight lanes in the low 256 bits and the high halves above, stored with
  `vextracti64x4` (a store, no shuffle port): the eight words formed apart cost 12 more operations per 8 keys. Loop body 874 to 837
  instructions per 16 keys; `llvm-mca` −0.3 % (Ice Lake, Tiger Lake), −2.3 % (Sapphire Rapids), −1.0 % (Zen 4). Measured: curve part alone
  +2.1 %, engine +0.4 to +0.8 %.
* **Together**: linear search **268.6 to 263.5 instructions per key**, measured +0.9 % (2000 pairs; whole-process pairs +1.9 %); kangaroo
  as above. Same digests, candidates, steps and distinguished points.
* **Tried and dropped** (the patches stay outside the tree):
  * *two products interleaved operation by operation* (`pz_fev_mul2`, `sqr_add2`, `mul_add2`, the pairs of the linear step and of the
    pair kangaroo): in a microbenchmark two chains 79 to 72 ticks per pair of products with GCC (clang: no gain), but in the linear step the
    loop body grew from 908 to 962 instructions (stack accesses 78 to 113: two products in flight need 20 accumulators), `llvm-mca` +2.6 %
    on all four targets, the curve part −4.9 % (minima); in the pair kangaroo +10.4 instructions per step and −8.5 %;
  * *two chains in the batch inversion of the kangaroo* (as the linear search has since 10.2): the forward pass alone unchanged (−0.3 %), it
    is port-bound, not latency-bound; +2.9 instructions per step;
  * *the lane kangaroo skewed by one vector* (the inverse, the jump entry and the slope of vector `c − 1` computed before the rest of the
    step of vector `c`, the same arithmetic): 137.0 to 145.1 instructions per step (GCC rematerializes 29 constants per vector with
    `movabs` and `vpbroadcastq`), 142.0 unrolled by two with the products inlined; measured +2 to +7 % over the lanes, but only level with
    the pairs, which the program picks here; `llvm-mca` Ice Lake and Tiger Lake −5.7 to −8 %, Sapphire Rapids −0.5 to −1.8 %, **Zen 4 +0.2
    to +0.4 %**;
  * *the columns 5 and 6 of a product in two accumulators each* (the low halves apart, one addition to join them): a product in a chain 59 to
    52 ticks, but +2.6 instructions per kangaroo step and +2.3 per key of the linear search; measured lanes +3.4 to +3.9 %, pairs +0.9 to
    +1.2 %, inside the spread of the A/A control for the pairs; `llvm-mca` Sapphire Rapids +2.3 % (pairs) and +2.5 % (forward pass), Zen 4
    +0.5 %;
  * *the products of the pair kangaroo inlined* (`PZ_FEV_INLINE`): 152.6 to 151.6 instructions per step, +2.2 to +3.1 %, inside the spread.
* **No assembly was needed.** GCC emits the intrinsics of the products in source order (it has no scheduling pass before register
  allocation on x86), so the order of the source is the order of the stream, and every variant above was intrinsics; the kept changes
  remove operations and do not depend on an order. They build and pass with gcc 13 and clang 18, the MinGW-w64 build under wine with IFMA
  assumed, and with `tools/ifma_emul.h` (which masks the operands to 52 bits as the instructions do).
* **What is left.** On a core with two IFMA ports the linear search is at the floor of its ports in both phases; on one with one port the
  step is port-0-bound. Only the kangaroo keeps latency, a quarter of its backward pass; the pairs take most of it, and a third chain costs
  more registers than it saves (the skewed lanes and the four-vector form of 10.6 alike).
