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

**After 1.18.0: where the other half of the modelled gain went.** The A2000 measured 793 to 882 M keys/s for the two-pipe kernel (+11 %)
where the model T = max(2A, 2F, A + F + O) said -20 to -23 %. The tools are the same (NVRTC 12.0, `ptxas` and `nvdisasm` for sm_86), plus
the scheduling control words that `ptxas` writes into every SASS instruction, read by `tools/research/sass_sched.py`: the *stall count*
(the cycles before the same warp may issue again, which is how the fixed latencies of the dependencies reach the hardware: a wrong count
would give wrong results, not a slow kernel, so the counts are the latencies `ptxas` knows), the yield bit and the scoreboard barriers of
the loads and stores. The script rebuilds the executed trace of a pair of keys (the forward loop, the pair loop with its two calls of
`hash160_w2` expanded, the rare blocks skipped) and runs N warps of it on one SM partition: one instruction issued per cycle, each pipe held
two cycles per warp instruction, the private arrays at 500 cycles (they are in video memory on the reference card, 160 MiB for 10240
work-items at window 512), the table and constant loads at 35.

* **Latency, by the numbers of `ptxas`.** One warp alone runs `hash160_w2` in 3888 cycles in 1.17.0 (2030 instructions, 1790 of them with a
  stall of 2: the ALU pipe itself, floor 2 × 1911 = 3822) and in 3762 cycles in 1.18.0 (2393 instructions, 1397 at stall 1, 742 at 2, 144 at
  3, 101 at 4: the two pipes let consecutive instructions issue every cycle, but the chains are longer, and the floor of the ALU pipe is now
  2 × 1453 = 2906). So the hash of one warp got no faster, and it is 29 % above its own pipe floor; the field part of the pair loop is 1338
  instructions with 3861 stall cycles (2.9 per instruction: the carry chains). With four warps per partition (the register cap) the model gives
  **4558 cycles per 32 keys per partition for 1.17.0 (T = 4499) and 3786 for 1.18.0 (T = 3487), -17 %**; with `IMAD.WIDE` at two slots 4672 and
  4142 (-11 %). The idle issue cycles of 1.18.0 are 16.7 %, of which **0.5 % with no warp ready** (that is the latency) and 16 % with a
  warp ready but its pipe busy: the four warps run the same code and stay in phase, so during the hash the ALU pipe is the bound (2906
  against 1862 cycles of `IMAD`) and during the field part the FMA pipe is (1582 against 1044), and the phase by phase bound, 2 × 2906 + 1582
  + 234 = 7628 per two keys, is 3814 per 32 keys, 9 % above T. Hypothesis 1 of this round (the longer chains starve the scheduler) is not
  what the control words say, and the levers built on it were measured and dropped: the `e = d + t1` and `a = t1 + t2` sums of the SHA
  rounds back on `IADD3` (hash 2400 instructions, A 1470, F 921, one warp 3782 cycles, model 3825: +1 %); the hash inlined in the pair loop
  so `ptxas` could overlap it with the field products (loop of 6169 instructions = 98 KB of code, 28 bytes of spill, the stall sum of the
  trace 11189 against 11950: it interleaves little, model 3751, -0.9 %, for an instruction footprint that no longer fits an L1 instruction
  cache of the size of Turing's); two keys hashed in lockstep in one thread (not built: +30 registers for a latency that is 0.5 % of the
  issue cycles). Register bank conflicts are not the difference either: same-bank source pairs without a reuse flag in the hash 1257 in
  1.17.0, 1111 in 1.18.0.
* **What the card measured against the model.** 793 M keys/s is 6100 cycles per 32 keys per partition at the 1.89 GHz of the probes, 882 M
  is 5485: **1.34 and 1.45 times the model**. Nothing in the SASS grew by that factor, and the ratio grew with the share of the issue slots in
  use (61 to 83 %). A sustained clock below the probes' explains both at once: the model matches at **1.41 GHz for 1.17.0 and 1.30 GHz for
  1.18.0**, which is what a laptop GA106 does under its power limit when the pipes get busier (the probes of `bench` run 0.2 s each, after
  the kernels, and the campaign numbers of 10.5 already showed the card losing 10 % to the CPU threads through the shared budget). This is
  the first thing to check on the card: `nvidia-smi --query-gpu=clocks.sm,clocks.max.sm,power.draw,power.limit,temperature.gpu,clocks_throttle_reasons.active --format=csv -l 1`
  while `pzsolve bench --gpu-only` runs its linear search (4 s) and then during a hunt. If `clocks.sm` sits at 1.3 to 1.4 GHz with a power
  throttle reason, the kernel is power-bound and the remaining gains are in the work per key, not in the cycles; if it holds 1.8 GHz, the
  1700 cycles between model and card are in something this machine cannot see (the instruction caches, the operand collector), and the
  variants below say which.
* **Kept.** The first denominator (i = 1, whose inverse is what is left of the batch product) and the centre (i = 0) were cases inside the
  loop over the pairs, and `ptxas` copied `inv` into `im` and the centre into x3 and y3 ahead of the tests on i on every iteration: 24
  moves per pair. Peeled out of the loop: the field part of the pair loop 1338 → 1302 instructions, the forward loop 183 → 191 (moves the
  compiler placed differently), **3155 → 3141 per key**, model 3786 → 3761; the capped variant spills 88 bytes outside the loops over the
  keys (none in them), the free one takes 188 registers instead of 172 (still two blocks per SM). Same arithmetic, same results.
* **More warps.** With blocks of 128 threads the occupancy moves in steps of a block: 5 blocks need 102 registers, which `ptxas` rounds to
  **96**, 6 blocks 85 → 80, so caps of 112 or 104 buy nothing. At 96 registers (`__launch_bounds__(128, 5)`) the kernel spills 240 bytes, all
  outside the loops over the keys (the executed counts of the pair loop, the forward loop and the hash are unchanged); at 80 it spills 332
  bytes into the pair loop (1338 → 1398) and the inversion (590 → 607): not offered. The model gives 3717 cycles with five warps (-1.8 %) and
  3664 with six; on a card whose warps drift out of phase it is worth more, on one that is power-bound nothing, so the 96-register cap is a
  tuner variant for windows 256 and 512 (`linear256m5`, `linear512m5`), together with a window of 1024 with either cap (half the inversions,
  about 0.6 instructions per key, 32 KiB of private memory per work-item; the model cannot tell them apart). The tuner measures all ten and
  keeps the default unless one is more than 3 % faster; `bench --retune` shows the table. Expected on the A2000, from the model: window 512
  with the 128-register cap 890 to 900 M keys/s (the peel), the 96-register cap 905 to 920 if the card is not power-bound, 1024 within 1 %
  of 512.
* **The form as a variant.** The two pipes of 1.18.0 are a fact of GA10x and AD10x: a partition issues `IMAD` to one 16-lane datapath and
  the other integer instructions to another. A card whose lanes all do both (unified datapaths, as Blackwell's are described) is bound by
  A + F + O, and there the one-pipe form of 1.17.0 (every addition an `IADD3` or a `LEA.HI`, the carries as `SEL`) is 12 % fewer
  instructions per key. Both forms are therefore compiled (`PZ_ONE_PIPE`, the `p4` and `p5` modules of modules.txt, for the windows 256,
  512 and 1024 with either cap) and the tuner measures the one-pipe twins of the two fastest two-pipe variants, so a card picks its form
  without anyone reading its ISA. On the A2000 the expectation is the two-pipe form by about 10 %, as measured; on a unified card the
  one-pipe form by up to 12 %.
* **The campaign loss** (882 in `bench`, 775 with 16 CPU threads). The thread that serves the card does, per launch of about 0.35 s (300
  M keys at this speed): a claim from the scheduler, three `k·G` on the host, a 264-byte write, the launch, a blocking wait on the context
  and an 8-byte read; under a millisecond of host time and one wake-up of a sleeping thread, so even a scheduler quantum of 15 ms on a
  saturated Windows machine is 4 % at the very most, and with the high priority the first-run measurement chose on this machine far less. The
  first-run measurement tries 16, 15 (one thread fewer per CUDA device), 12, 8, 4 and 0 threads, each after 2 s of settling over an 8 s
  window: power sharing acts over longer times (10.5), which is why `-t 14` and `-t 8` were a wash at two minutes but the UHD cost 90 M.
  What to measure on the laptop, with `--gpu 0`: `-t 16`, `-t 15` and `-t 14` for ten minutes each, reading `clocks.sm` of the card and
  the package power of the CPU at the same time; the configuration whose GPU clock is highest at equal total is the one to keep. Double
  buffering the launches (a second hit buffer, the next launch enqueued before the results of the previous one are read) would remove the
  host turnaround entirely; it is worth doing only if the measurement above shows the card idle between launches rather than slow.
* **What is left in the SASS.** The hash is at the floor of its expression on this ISA (the part after 1.17.0 above); per pair of keys the field part
  keeps 48 `IMAD.MOV` and 6 `MOV` (half of them zero the odd register of an addend pair whose high half is empty: the price of the aligned
  `IMAD.WIDE` pairs, 2 instructions either way), 2 `SEL`, 12 `LOP3` (the borrow masks), 71 `IADD3` and 398 `IADD3.X` (the carry chains). The
  products are 3.5 multiplications and one square per key (7M + 2S per pair: 3 for the batch inversion, which is Montgomery's minimum, 2M + 1S
  per point); sharing work between λ₊² and λ₋² (their difference is 4·sy·yc·im²) costs a multiplication to save a square. The window of 1024
  removes 0.6 instructions per key of the inversion; nothing else in the per-key count depends on the window.

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
