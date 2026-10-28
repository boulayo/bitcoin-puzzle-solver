# Research log: what was tried and what was measured

This file keeps the development history behind [`ALGORITHMS.md`](ALGORITHMS.md): the variants that were tried and discarded, the intermediate
measurements and the analyses of designs that the program no longer uses (the centered Gaudry-Schost layout, schemas 1 and 2 in detail, Karatsuba, the
herd of walkers in shared memory or in L2, the precomputation study, the counting of pairs with integer values). What the program does today is in
ALGORITHMS.md. The text was moved from there without changes, apart from the file name added to the cross-references; the figures are those of the
version named in each section, and several are superseded by later sections (the corrections are noted where they are).

**Section numbers.** The sections keep the numbers they had when everything was in one document, because they are cited from the source, the tools and
the other documents. Here are 4.5, 4.6 and 8.4 to 8.10; in ALGORITHMS.md are the others and a short pointer to each section that is here. Sections
4.5, 4.6, 8.6, 8.7 and 8.9 are only here. Sections 8.4, 8.8 and 8.10 are in both files: the description of the schema in use (8.8, 8.10) and the
arithmetic minimum of a step (8.4) stay in ALGORITHMS.md, and the note at the start of each section here says what is in the other file. A reference
written as "ALGORITHMS.md section N" or "RESEARCH.md section N" names the file; one without a file name is to a section of the same file. The term
"schema" is explained at the start of section 4 of ALGORITHMS.md.

## Contents

4. The interval kangaroo
   * 4.5 Measurements (real curve, `pzsolve kstat BITS RUNS THREADS`)
   * 4.6 Things evaluated and rejected
8. Theoretical limits: how much can still be improved?
   * 8.4 Cost of a step and hardware bottlenecks (1.5.0)
   * 8.6 Modular multiplication: what was tried
   * 8.7 How low can it go: counting pairs with integer values
   * 8.8 Below 1.085: species with fixed residues (schema 2)
   * 8.9 More RAM or a precalculation: what can be bought
   * 8.10 Local walks, long jumps and banded zones (schema 3)

## 4. The interval kangaroo (public key known)

### 4.5 Measurements (real curve, `pzsolve kstat BITS RUNS THREADS`)

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
  takes `≈ 6.2·√W` bytes (25 GiB at `W = 2^64`, 98 GiB at `2^68`), so it is usable only for medium-small intervals.
* **4-animal kangaroo** (≈ 1.715·√W) and Gaudry–Schost without symmetry (≈ 1.66·√W): superseded by the symmetric variant.
* **Parameters of the centered variant (up to 1.5.0, `tools/research/ksim.c`, 8000 runs per point, W = 2^28, error ≈ ±0.007)**: a grid of 45
  combinations of wild zone `B ∈ {1/16…1/4}·W`, tame fraction `{0.4, 0.5, 0.6}` and mean jump `{0.3, 0.5, 0.8}`, plus 4 widths of the
  tame zone. The point in use (`A = W/2`, `B = W/8`, half tame, jump 0.5) is the best (1.292·√W in the model); the optimum is flat and
  far-off choices cost up to 7% (tame on `0.75·W`: +17%). No room *in that family*: the gain came from changing the layout (ALGORITHMS.md section 4.2). In the model, 3000–4000 runs per point (error ≈ ±0.01), same number of
  walkers and `dp`: centered 1.27–1.36·√W, new layout 1.14–1.23·√W on `W = 2^26 … 2^34` and 64–2048 walkers
  (`ksim rck=1 tf=0.3333 jabs=12`).
* **Third species of walkers on `2x`** (wild with exponent `±2x + d`, starting from `2Q`, each collision with different coefficients gives
  `(c₁ − c₂)·x = d₂ − d₁`): 27 combinations of shares and widths, the best costs 1.365·√W, that is 6% **more**; more walkers
  on `2x` make it worse (up to 1.78). Their points often fall outside the tame zone and the extra pairs do not compensate.
* **Distribution of the solved keys**: the relative position in the interval of the 82 known keys is compatible with uniform
  (Kolmogorov–Smirnov D = 0.074, 5% threshold 0.15; mean 0.51; one-bits 49.3%): no part of the interval is more likely.

## 8. Theoretical limits: how much can still be improved?

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
  ALGORITHMS.md section 4.2). Tame–tame: none. With coefficients 2 or more the pairs are worth on average `1/2`, `1 + 1/3`, … and **no congruence makes
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
give `2^−9.5` ≈ 0.1 %. **The expected cost at real scales is therefore ≈ 1.09, not 1.15** (*corrected in 8.10: this holds only with short walks
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
The open puzzles with a public key are 6 (135, 140, …, 160), with widths from `2^134` to `2^159`. The smallest dominates the cost and the others
do not help it. The same holds for multiple targets without a table (ALGORITHMS.md 8.2): with widths so different the gain is negligible.

**4. Other ways of using RAM to go faster.**
* *Cheaper step*: every new point requires at least one addition on the curve. Tables help only the starting points (the comb of
  `ec_mul_g`, already in use) and the sharing of the inversion (already done over 4096 walkers per CPU thread and over those of a GPU work-item, section 8.4).
* *More memory for distinguished points*: it lowers `dp_bits` and so the delay with which a collision is recognized. At real scales that delay
  is already worth ≈ 0.1 % (8.7).
* *"Free" points*: with a single inversion one obtains both `P + J` and `P − J`, as in the giant steps of BSGS (ALGORITHMS.md section 9). In a walk the
  second point lies on no track: it is useful only if it is itself a distinguished point, i.e. once in `2^dp_bits`. Not worthwhile.

**Conclusion.** With the resources of one computer (or of many) the factor that matters is that of the kangaroo, ≈ 1.02 at large scale with schema 3 (8.10). RAM does not
lower it appreciably and precomputation raises it. The levers left are those of the step (speed per step, sections 8.4–8.6) and, for a few
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
