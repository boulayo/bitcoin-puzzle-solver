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
  takes `≈ 6.2·√W` bytes (25 GiB at `W = 2^64`, 98 GiB at `2^68`), so it is usable only for medium-small intervals. With the kangaroo
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
  version measured 368). Estimate: ≈ 11% fewer instructions per key. The logic of the chains is verified with an
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

## 8. Theoretical limits: how much can still be improved?

### 8.1 The constant: the figures of the earlier layouts

ALGORITHMS.md section 8.1 gives the constant of the layout in use. Its table, when the earlier layouts were still in the program, gave for
this implementation: **schema 3 (1.7.9): 1.06 measured from 2^32 to 2^44, ≈ 1.02 on the open puzzles (model, 8.10)**; schema 2 (1.7.8): ≈ 1.045
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
instructions) was ≈ 430 per step at NK = 128 (a fifth of the total) and with the divstep (≈ 10,500) is ≈ 80. At the integer throughput observed on the
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
  | schoolbook (in use) | 184 |
  | additive Karatsuba | 248 (+35 %) |
  | subtractive Karatsuba | 256 (+39 %) |
  | by columns, one `IMAD.WIDE` and two sums per product | 232 (+26 %) |
  | by rows, one carry chain per row | 176 (−4 %) |

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
points, so the coverage is random and the value to beat is 1.085. Where memory suffices (`W ≲ 2^68`) there is BSGS (ALGORITHMS.md section 9): table
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
| kangaroo, schema 3 (8.10) | ≈ 1.02 (model, open puzzles) | the table of distinguished points: negligible |
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
  search ≥ ≈ 2.3·W** at every size. For a single key the minimum total is `2·√(2.3·W) ≈ 3.0·√W`, three times the kangaroo (1.02–1.06).
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
`P + m·S` with `P·S ≈ 2.3·W`. The minimum is `2·√(2.3·m·W)`, i.e. `≈ 3.0·√(W/m)` per key, and it beats the kangaroo (≈ 1.02·√W per key) from **about 9 keys on**.
The open puzzles with a public key were 6 when this was written (135, 140, …, 160; 135 has since been solved), with widths from `2^134` to `2^159`. The smallest dominates the cost and the others
do not help it. The same holds for multiple targets without a table (ALGORITHMS.md 8.2): with widths so different the gain is negligible.

**4. Other ways of using RAM to go faster.**
* *Cheaper step*: every new point requires at least one addition on the curve. Tables help only the starting points (the comb of
  `ec_mul_g`, already in use) and the sharing of the inversion (already done over 4096 walkers per CPU thread and over those of a GPU work-item, section 8.4).
* *More memory for distinguished points*: it lowers `dp_bits` and so the delay with which a collision is recognized, about 1 to 2
  `K·2^dp_bits/√W` (8.7). When memory sets `dp_bits` that delay is several percent: in the model of 1.12.0 (CHANGELOG), puzzle 140 with 2^22
  kangaroos costs 1.10·√W at 32 GB and 1.15 at 16 GB, against ≈ 1.02 without the delay. Entries of 16 bytes instead of 32 (the table keeps
  the index of the record in `dp.bin` and reads the distance back on a match of two kinds, after 1.12.0) have the effect of twice the RAM:
  1.08 at 32 GB, 1.10 at 16 GB, 1.15 instead of 1.23 at 8 GB. Entries of 8 bytes (the tag alone, with a scan of `dp.bin` on every match of two
  kinds) would give 1.07, 1.08 and 1.10, for a scan of a file of tens of GB, under the table lock or in the background, at every false match:
  not done. More RAM buys that back, and no more: the factor of the walk itself does not move. (The ≈ 0.1 % of 8.7 assumed `dp ≈ 40` with `K ≈ 2^20` and the RAM for that table.)
* *"Free" points*: with a single inversion one obtains both `P + J` and `P − J`, as in the giant steps of BSGS (ALGORITHMS.md section 9). In a walk the
  second point lies on no track: it is useful only if it is itself a distinguished point, i.e. once in `2^dp_bits`. Not worthwhile.

**Conclusion.** With the resources of one computer (or of many) the factor that matters is that of the kangaroo, ≈ 1.02 at large scale with schema 3 (8.10). Beyond the delay
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
