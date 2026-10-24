# pzsolve algorithms

This document explains *what* the program computes and *why* it is efficient. For the structure of the code see
[`ARCHITECTURE.md`](ARCHITECTURE.md), for usage see [`USAGE.md`](USAGE.md).

Sections 1 to 7 cover the problem, the linear search, the seed-based visiting order, the interval kangaroo (in its three
successive schemas), the GPU kernels, rough running times and references. Section 8 is the theory: what is proven in the literature,
what was measured, what is only modelled, and how much improvement is still possible. Sections 9 to 11 cover the BSGS mode, the SIMD
lane arithmetic and the time-estimation model. Numbers are labeled as measured on the real curve, measured in a model
(`tools/ksim.c`), or estimated.

## Contents

1. The problem
2. Linear search (address only)
   * 2.1 Arithmetic
   * 2.2 Vectorized hash160
   * 2.3 Edge cases
3. Unique visiting order (seed)
4. The interval kangaroo (public key known)
   * 4.1 Setup (from 1.6.0)
   * 4.2 Why it costs less
   * 4.3 Unproductive cycles and drift
   * 4.4 Parameters (chosen automatically)
   * 4.5 Measurements (real curve, `pzsolve kstat BITS RUNS THREADS`)
   * 4.6 Things evaluated and rejected
   * 4.7 Resuming
5. GPU (CUDA and OpenCL)
6. How long it takes (orders of magnitude)
7. References
8. Theoretical limits: how much can still be improved?
   * 8.1 The constant in front of sqrt(W)
   * 8.2 The exponent
   * 8.3 Summary
   * 8.4 Cost of a step and hardware bottlenecks
   * 8.5 Comparison with the state of the art: puzzle 135 and RCKangaroo
   * 8.6 Modular multiplication: what was tried
   * 8.7 How low can it go: counting pairs with integer values
   * 8.8 Below 1.085: species with fixed residues (schema 2)
   * 8.9 More RAM or a precalculation: what can be bought
   * 8.10 Local walks, long jumps and banded zones (schema 3)
9. Symmetric BSGS with a RAM table (`--mode bsgs`, from 1.1.0)
   * 9.1 Algorithm
   * 9.2 How it is implemented
   * 9.3 When it pays off
10. SIMD lane arithmetic (`avx2v`, `avx512v`, `avx512i`)
    * 10.0 Lane kangaroo (1.5.0, `src/kang_vec_impl.inc`)
    * 10.1 AVX-512 IFMA variant (`avx512i`, `lib/pzcore/include/pzcore/fev52.h`)
11. Time-estimation model (`src/eta.c`)

## 1. The problem

For puzzle *n* the private key `k` lies in `[2^(n-1), 2^n − 1]` (an interval of width `W = 2^(n-1)`), and the address is
`Base58Check(0x00 ‖ RIPEMD160(SHA256(compressed pubkey)))` with `pubkey = k·G` on the secp256k1 curve.

* If **only the address** is known, the only known method is to try keys: the mean cost is `W/2` evaluations of
  `k → hash160(k·G)`.
* If the **public key** `Q = k·G` is known (revealed by an outgoing transaction), the problem becomes a
  *discrete logarithm in an interval*, solvable in about `c·√W` group operations (kangaroo algorithms).

## 2. Linear search (address only)

### 2.1 Arithmetic

* Field `GF(p)`, `p = 2^256 − 2^32 − 977`, in 4 limbs of 64 bits with lazy reduction (`2^256 ≡ 2^32 + 977`).
  The 4×4 multiplication is inline assembly with **MULX** and the two carry chains **ADCX/ADOX** (ADX); a portable C version
  serves CPUs without BMI2/ADX (the two are compared in `lib/pzcore/tests/t_fe.c` on 400,000 pairs, edge cases included).
* Consecutive keys of a block are obtained by **adding affine points** instead of redoing a scalar multiplication for every
  key. An affine addition needs one modular inversion, which costs about as much as 270 multiplications: it is amortized with
  **Montgomery's trick** (a single inversion for a whole vector of denominators).
* **Symmetric group.** For a center `C = c·G` and the table `i·G` (i = 1…H), the denominators `x(i·G) − x(C)` serve both
  `C + i·G` and `C − i·G` (same x-coordinate, opposite y): one inversion for `2H` consecutive keys, plus the next
  center `C + 2H·G`. With `H = 256` (CPU) the cost is ≈ `3.5 M + 1 S` per key (M = field multiplication, S = field squaring)
  plus 1/512 of an inversion.
* The hash needs the parity of `y`: it is obtained by also computing `y` (one multiplication) instead of hashing twice.

### 2.2 Vectorized hash160

SHA-256 (a single block: 33 bytes + padding) and RIPEMD-160 (a single block: 32 bytes + padding) are written once with GCC's
*vector extensions* and instantiated with 4 lanes (SSE2/NEON), 8 lanes (AVX2) and 16 lanes (AVX-512). The engine to use is
chosen at runtime with a 0.25 s micro-benchmark (`--isa` forces it). For each batch only the **first word** of the digest is
compared with the target; the rare candidates are verified in full with the scalar hash and then verified again by
recomputing `k·G` from scratch.

### 2.3 Edge cases

Zero denominators (tiny keys, e.g. center = 2·512) zero out the batch product: the condition is detected with a single
check per group and the group is recomputed exactly (one scalar multiplication per key). Blocks that start below 2^22 are
always scanned on the CPU, even when a GPU is present. Keys beyond the boundary of a block are never assigned twice: the
scheduler knows the exact length of every block (the last one too, which is shorter in custom intervals).

## 3. Unique visiting order (seed)

The interval is divided into `M = ⌈W / 2^B⌉` blocks of `2^B` keys (default `B = 36` for large intervals, smaller for small puzzles). The campaign
visits the blocks in the order `π(0), π(1), π(2), …` where `π` is a **pseudo-random permutation of [0, M)** determined by the seed.

* `π` is a Feistel network (8 rounds) with round function `SHA-256(seed ‖ round ‖ half)`, on `⌈log2 M⌉` bits; domains that are not
  a power of two are handled with *cycle-walking*. It is a **bijection**: every block is visited exactly once, and it is
  computed in O(1) (a few SHA-256 per block, negligible next to the 2^32 keys of a block). Its inverse lets
  `pzsolve where` say *when* a key will be reached.
* **Why not modular arithmetic with a large prime** (`i ↦ a·i + b mod p`): it is a valid and very fast permutation,
  but successive blocks end up in arithmetic progression (a few observations reveal the step, and two campaigns with correlated
  multipliers overlap in a structured way). The Feistel network is just as cheap and statistically
  indistinguishable from a random permutation.
* Inside a block the scan is sequential (the most efficient form). Progress is kept as a set of
  completed intervals in the "virtual space" `V = position_in_path · 2^B + offset`: there are at most as many holes as work requests
  in flight, so the checkpoint stays a few lines long even after months (`state.dat`, with checksum).
* The seed has 256 bits (`getrandom` + time + pid + host, hashed); `--seed` accepts 64 hex digits (used as they are) or
  any text (hashed).

* **Several computers (`--part`, from 1.7.5)**. The *positions* of the visiting order are split, not the interval: with `T` shares, the computer
  that holds the shares `r₁ < r₂ < … < r_m` scans the blocks at positions `k` with `k mod T ∈ {r₁,…,r_m}`, in its local order
  `j ↦ ⌊j/m⌋·T + r_{j mod m}`. The positions of the computers are disjoint and `π` is a bijection, so the blocks (and the keys) are
  disjoint and cover everything, with no communication. Since all blocks are equal (except at most the one at the end of the interval), a
  share is proportional to work: a computer twice as fast only needs twice as many shares (`pzsolve split`). Shares are dealt out in turns, not
  as contiguous slices of the interval, so every computer follows the same random order and coverage grows uniformly
  over the whole interval. The number of blocks of a part is `q·m + #{r_i < M mod T}` with `q = ⌊M/T⌋`; the exact length in keys
  accounts for the short block, which is found with `π⁻¹` of its index.

**What it does NOT do**: it does not make the key more likely to be found. In a uniform interval every order has the same probability of
success per scanned key. The random order only avoids duplicating the work of those who start from the beginning (or the
end) of the interval, and makes each campaign independent of the others.

## 4. The interval kangaroo (public key known)

**Which schema the program uses.** The kangaroo exists in three schemas. They share the step and the equivalence classes and differ in the
species of walkers, where they start, the length of the jumps and how a pair of distinguished points is resolved. **New campaigns use schema 3**
(from 1.7.9: the species and residues of schema 2, long jumps of mean `W^¾/16` and start zones split into bands; sections 8.8 and 8.10).
Older campaigns keep the schema they started with, because changing the jumps would make the distinguished points already found useless: schema 2
for campaigns started with 1.7.8, schema 1 for those started with 1.6.0 to 1.7.7. This section describes schema 1, which is the simplest and the base of
the later ones: 4.1 to 4.6 (setup, parameters, measurements) refer to it, while 8.8 and 8.10 give the changes of schemas 2 and 3 and their
measured cost (≈ 1.10 and ≈ 1.06 against 1.15·√W).

### 4.1 Setup (from 1.6.0)

This subsection describes **schema 1**, used by the campaigns started from 1.6.0 to 1.7.7 (which keep it). New campaigns
from 1.7.8 use schema 2: same classes, same step, but three species of walkers with fixed residues mod 12 (section 8.8), about 4% fewer
steps; from 1.7.9 they use schema 3 (section 8.10).

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

### 4.2 Why it costs less

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
  (section 4.5), about **10% fewer steps**, consistent with the K = 1.15 declared by RCKangaroo.

### 4.3 Unproductive cycles and drift

Walking on the classes `{P, −P}`, a walker can return to a point it already visited without ever meeting a DP: from then
on it repeats the same points forever (an *unproductive* cycle). With `R` jumps in the table (Bos–Kleinjung–Lenstra, *On the use
of the negation map in the Pollard rho method*, 2010):

* **Cycles of length 2** (`P → P' → P`): probability `1/(2R)` per step. When the current point coincides with the one from
  two steps earlier, the next jump `(j+1) mod R` is used: the rule depends only on the history `(P_{n−1}, P_n)`, so two
  walkers that merge keep merging.
* **Cycles of length 4, 6, …**: even after removing those of length 2 they remain, with probability `≈ (R−1)/(4R³) ≈ 1/(4R²)` per step,
  that is **one every ≈ 2.6·10⁵ steps** with `R = 256` (measured: 3·10⁵ in the model of `tools/ksim.c`, almost all of length 4).
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

### 4.4 Parameters (chosen automatically)

Values for schema 1; schemas 2 and 3 change the walker zones and the jump table (sections 8.8 and 8.10). Here `K` is the total number of walkers.

| Parameter | Value | Reason |
|-----------|-------|--------|
| base | start − `W/32` | the interval on one side of zero, far from the folding |
| tame zone | `[1, W/32)`, one third of the walkers | as in RCKangaroo; in the simulator 1/3 is better than 1/5 and 1/2 |
| wild zone | `−x + v`, `v` even in `[0, W + W/16)` | the wild kangaroos cover the whole interval on both sides of zero |
| jumps | 256 even, uniform in `[2, 24·√W]` (+16 for cycle escape) | symmetric walk, even jumps for wild–wild collisions |
| walkers per CPU thread | `clamp(√W/(256·threads), 16, 4096)`, multiple of 16 | the batch inversion is shared by the whole herd of the thread; 4096 is the measured optimum (sec. 8.4) |
| GPU walkers | `NK` per work-item (64/128/256, tuned) × 512 work-items per SM (CUDA) | at most `√W/256` in total, so each one walks long enough |
| `dp_bits` | `max(⌊log2(0.01·√W/K)⌋, bits required by RAM, 2)` | more RAM, less overhead |
| DP table | 32 bytes/entry, grows up to the `--mem` limit (default 40% of free RAM, max 64 GiB); if the limit is reached the density of stored points is halved (only DPs with one more zero bit are kept) instead of stopping | a single mutex: DPs are rare |

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
  step covers `2T+1` values: mean cost `T + W/(4T)`, minimum `1.0·√W` for `T = √W/2` (worst case `1.41·√W`, or `1.5·√W` with this `T`, see 9.1). **With enough RAM
  it therefore beats the kangaroo (1.15·√W)**. It has been implemented since 1.1.0 (`--mode bsgs`, section 9); the table
  takes `≈ 6.2·√W` bytes (25 GiB at `W = 2^64`, 98 GiB at `2^68`), so it is usable only for medium-small intervals.
* **4-animal kangaroo** (≈ 1.715·√W) and Gaudry–Schost without symmetry (≈ 1.66·√W): superseded by the symmetric variant.
* **Parameters of the centered variant (up to 1.5.0, `tools/ksim.c`, 8000 runs per point, W = 2^28, error ≈ ±0.007)**: a grid of 45
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

### 4.7 Resuming

Every new DP is appended to `dp.bin` (32 bytes + a header with a fingerprint of the interval and the key). On resume the table is
rebuilt and the walkers restart from new points (a different random stream for each session); the old trails
remain useful because each of their later DPs is already in the table. A file from a different campaign is recognized and refused.
From 1.7.8 new campaigns used schema 2 and the log format 3 (`"PZDP3"`, section 8.8); from 1.7.9 they use schema 3 and the format `"PZDP4"`
(section 8.10); campaigns with a log in format 1 or 2 stay on schema 1. Since 1.6.0 the base has changed (section 4.1): a `dp.bin` from 1.5.0 or earlier is converted at the first
resume (the points stay the same; the distances of the wild kangaroos are rewritten relative to the new base, `d' = d − σ·(c − b)`), with a
self-test that checks it on the curve.

## 5. GPU (CUDA and OpenCL)

* The kernels are written **once** in OpenCL C 1.2 with a field of 8 limbs of 32 bits. For OpenCL the driver compiles them from the embedded
  sources; for CUDA `tools/gen_ptx.py` compiles them with NVRTC (through `src/kernels/pz_cuda.h`, which translates the dialect) into PTX for
  `compute_52`, embedded in the executable and compiled by the NVIDIA driver for the installed card (result cached). Both runtimes
  are loaded dynamically (`dlopen` / `LoadLibrary`): no build-time dependency and no SDK at runtime.
* With CUDA: 128 threads per block (`__launch_bounds__(128)`), grid rounded up and extra work-items that exit immediately
  (each kernel receives the number of work-items as its last argument); hash160 and modular inversion are compiled once as real
  functions (smaller PTX and fewer registers: 128 for the linear kernel on sm_86, no significant spill); the context waits
  for the kernels to finish without busy-waiting, so the core is left to the CPU compute threads.
* **CUDA arithmetic (from 1.3.0).** Measured on the sm_86 (Ampere) code: per key the linear kernel executes ≈ 4200 integer
  instructions, half in hash160 (≈ 2100, already close to the optimum) and half in the field. At ≈ 513 M keys/s the reference laptop GPU (20 SM, 4 GB, 192 GB/s, 2 MiB of L2; the card on which the GPU speeds in this document were
  measured) is at the limit of its integer throughput (20 SM × 64 operations per cycle): more threads are of no use, what matters is the instruction count. For this reason
  addition, subtraction and the reduction after a product use PTX carry chains (`add.cc`/`addc`, `sub.cc`/`subc`,
  `mad.lo.cc`/`madc.hi.cc` for the multiplication by 977): add+sub from 176 to 88 instructions, multiplication from 328 to 288, a dedicated
  square (36 products instead of 64) from 328 to ≈ 232. The 8×8 product stays in C because Ampere has no native `madc.hi` (the all-PTX
  version measured 368). Estimate: ≈ 11% fewer instructions per key. The logic of the chains is verified with an
  exact emulation of the PTX semantics (`tests/t_gpufe.c`, 5 million combinations, edge cases of the double carries; mutation testing);
  on the device it is confirmed by the self-test.
* **Variants of the linear kernel (CUDA)**: window 128, 256 or 512 × free registers (≈ 160) or limited to 128 (`__launch_bounds__(128, 4)`:
  more resident warps). Tuning measures all six on the device and keeps the fastest.
* **Linear kernel with "fused" inversion (1.5.0)**: the backward pass of Montgomery's trick derives each inverse and uses it
  immediately for the points `C ± i·G`, recomputing the denominator from the table instead of keeping it in an array. The private memory per
  work-item is halved (window 256: from 17 to 8.7 KB) and the traffic to video memory drops from ≈ 96 to ≈ 32 bytes per key
  (at ≈ 600 M keys/s it was ≈ 57 GB/s out of ≈ 190). The hash filter uses the first three words (96 bits; every candidate is re-verified
  on the CPU anyway): they do not depend on the last steps of RIPEMD-160, which the compiler eliminates (≈ 0.4% of instructions).
* **Linear**: each work-item scans `ngroups` consecutive groups from its own center `P0 + gid·D` (computed on the
  device with a small double-and-add multiplication); groups of `2·HALF` keys, table `i·G` in constant memory, complete hash
  160 on the device, hits collected with an atomic counter. The duration of a launch is regulated to ~0.35 s.
  `HALF` is a compile-time constant of the kernel: **64, 128 or 256**; on the first run on a real GPU the program tries all
  three (a few seconds each), picks the fastest (it needs an advantage of > 3% over 128) and stores the choice in
  `$XDG_CACHE_HOME/pzsolve/gpu_tune.txt` (key: name, driver version, compute units, program version).
  `PZ_GPU_HALF=N` forces it, `PZ_GPU_TUNE=0` disables tuning; `pzsolve selftest` verifies all the variants.
* **Kangaroo (rewritten in 1.5.0, kernel v2 in 1.7.0)**: up to 1.4.0 each work-item handled **16** walkers: one inversion (≈ 270 multiplications)
  every 16 steps, that is ≈ 17 multiplications per step against the ≈ 6 of the step itself, and the kernel used 255 registers with spill.
  Now `NK` = 64, 128 or 256 walkers per work-item (tuning on the device, `PZ_GPU_NK` to force it), the same "fused" scheme
  as the linear kernel (no array of denominators), **128 registers without spill** and therefore 512 resident work-items per SM. State in
  global memory (128 bytes per walker: x, y, 192-bit distance, type, sign, 32-bit anti-cycle labels, last DP);
  the DPs go out in a buffer and are inserted in the same table as the CPU's. Restarts are handled by the host.
  **Kernel v2 (1.7.0)**, built around memory traffic (sec. 8.4): (a) the distance does not travel from step to step: every
  step adds 16 bits to a *history* buffer (jump index, "negated" bit) and `d` is rebuilt by replaying the history, at the end of the
  launch for everyone and, for the rare distinguished points, on the spot; (b) one pass per round instead of two: the pass that moves the
  walkers also computes the denominators of the next round and their running products, which alternate between prefixes and suffixes (the pass
  goes down, then up, ...): Montgomery's trick works with both, and each pass reads and overwrites the products
  left by the previous one; (c) the inversion is by divstep (sec. 8.4); (d) a launch does at most 64 rounds (depth of the history),
  starting from the step number common to the whole herd.
* **Verification**: at startup every device runs (a) field operations, hash160 and point additions compared with the CPU,
  (b) a linear search with a planted key, (c) for the kangaroo **the same group of walkers run on GPU and on CPU,
  with a bit-by-bit comparison of states, DPs and restarts** (with a low step limit, so that restarts really happen). A device
  that fails is excluded; an NVIDIA card that fails through CUDA is retried through OpenCL. Every key found is
  re-verified on the CPU anyway.
* Performance: measured on the reference laptop GPU up to 1.4.0; the 1.5.0 kernels must be re-measured with `bench` on other cards. Functional verification: OpenCL on pocl (OpenCL on CPU); CUDA with a simulated driver that
  runs the same kernel sources behind the driver APIs (`tests/mock_cuda/`), while the translation of PTX into GPU code by the real driver is
  covered by the self-test at startup. `ptxas` accepts all the modules for sm_86.

## 6. How long it takes (orders of magnitude)

Linear search, **mean case** (half the interval), at constant speed:

| puzzle | keys (mean) | 100 M/s (CPU) | 1 G/s | 10 G/s |
|--------|-------------|---------------|-------|--------|
| 50 | 2.81e+14 | 32.6 days | 3.26 days | 7.82 hours |
| 60 | 2.88e+17 | 91.3 years | 9.13 years | 334 days |
| 66 | 1.84e+19 | 5.8e+03 years | 585 years | 58.5 years |
| 70 | 2.95e+20 | 9.4e+04 years | 9.4e+03 years | 935 years |
| 71 | 5.90e+20 | 1.9e+05 years | 1.9e+04 years | 1.9e+03 years |
| 75 | 9.44e+21 | 3.0e+06 years | 3.0e+05 years | 3.0e+04 years |
| 80 | 3.02e+23 | 9.6e+07 years | 9.6e+06 years | 9.6e+05 years |
| 90 | 3.09e+26 | 9.8e+10 years | 9.8e+09 years | 9.8e+08 years |
| 100 | 3.17e+29 | 1.0e+14 years | 1.0e+13 years | 1.0e+12 years |
| 130 | 3.40e+38 | 1.1e+23 years | 1.1e+22 years | 1.1e+21 years |
| 135 | 1.09e+40 | 3.5e+24 years | 3.5e+23 years | 3.5e+22 years |
| 160 | 3.65e+47 | 1.2e+32 years | 1.2e+31 years | 1.2e+30 years |


Kangaroo, **mean case** (≈ 1.15·√W steps from 1.6.0; ≈ 1.10·√W from 1.7.8, that is about 4% fewer than in the table; ≈ 1.06·√W from 1.7.9, section 8.10):

| puzzle | steps (mean) | 100 M/s (CPU) | 1 G/s | 10 G/s |
|--------|--------------|---------------|-------|--------|
| 60 | 9.64e+08 | 9.64 s | 964 ms | 96.4 ms |
| 70 | 3.09e+10 | 309 s | 30.9 s | 3.09 s |
| 80 | 9.87e+11 | 2.74 hours | 987 s | 98.7 s |
| 90 | 3.16e+13 | 3.66 days | 8.78 hours | 3.16e+03 s |
| 100 | 1.01e+15 | 117 days | 11.7 days | 1.17 days |
| 110 | 3.24e+16 | 10.3 years | 1.03 years | 37.4 days |
| 120 | 1.04e+18 | 328 years | 32.8 years | 3.28 years |
| 130 | 3.31e+19 | 1.0e+04 years | 1.0e+03 years | 105 years |
| 135 | 1.87e+20 | 5.9e+04 years | 5.9e+03 years | 594 years |
| 140 | 1.06e+21 | 3.4e+05 years | 3.4e+04 years | 3.4e+03 years |
| 150 | 3.39e+22 | 1.1e+07 years | 1.1e+06 years | 1.1e+05 years |
| 160 | 1.09e+24 | 3.4e+08 years | 3.4e+07 years | 3.4e+06 years |

Puzzles without a public key are out of reach for the hardware of a single computer from `n ≳ 70`; those with a known public
key from `n ≳ 110–120`. The program stays correct for any `n ≤ 190` (kangaroo) / `n ≤ 255` (linear),
but the time grows as `2^(n/2)` and `2^n`.

## 7. References

* S. Galbraith, R. Ruprai, *Using Equivalence Classes to Accelerate Solving the Discrete Logarithm Problem in a Short
  Interval* (PKC 2010): <https://eprint.iacr.org/2010/615>. Cost ≈ 1.36·√N with equivalence classes.
* S. Galbraith, J. Pollard, R. Ruprai, *Computing Discrete Logarithms in an Interval* (Math. Comp. 82, 2013):
  <https://eprint.iacr.org/2010/617>. 4-animal kangaroo ≈ 1.715·√N, improved Gaudry–Schost ≈ 1.661·√N.
* S. Galbraith, M. Holmes, *A non-uniform birthday problem with applications to discrete logarithms*:
  <https://eprint.iacr.org/2010/616>.
* P. van Oorschot, M. Wiener, *Parallel collision search with cryptanalytic applications* (J. Cryptology 1999): distinguished points.
* P. Gaudry, E. Schost, *A low-memory parallel version of Matsuo, Chao, and Tsujii's algorithm* (ANTS 2004).
* The abstract simulator used to tune the parameters is in `tools/ksim.c`.
## 8. Theoretical limits: how much can still be improved?

This section separates what is **known in the literature** from what is **the author's own reasoning** (marked as such).

### 8.1 The constant in front of √W (constant `q`)

| Algorithm | Memory | Mean cost in units of √W | Origin |
|-----------|--------|--------------------------|--------|
| classic kangaroo (van Oorschot–Wiener) | small | 2 | literature |
| 4 kangaroos / improved Gaudry–Schost | small | 1.715 / 1.661 | Galbraith–Pollard–Ruprai |
| Gaudry–Schost with `±` classes | small | ≈ 1.36 (theoretical) | Galbraith–Ruprai |
| **this implementation** | small (DP table) | **schema 3 (1.7.9): 1.06 measured from 2^32 to 2^44, ≈ 1.02 on the open puzzles (model, 8.10)**; schema 2 (1.7.8): ≈ 1.045 with short walks (8.8); schema 1: 1.08–1.21, mean 1.15 (measured, 1.6.0; before that 1.26), ≈ 1.09 with many walkers (model, 8.7); for schemas 1 and 2 with long walks see the correction in 8.10 | `pzsolve kstat`, `ksim` |
| BSGS with `±`, `T = √W/2` | `≈ 6·√W` bytes | **1.0** (worst case 1.41) | direct calculation (section 4.6) |
| counting lower bound | any | **≥ ≈ 0.67**, optimistic: with integer values the optimum of the model is 0.8165 (8.7) | the author's own reasoning, below |

**Is `q < 1` possible in theory? Only beyond a certain point.** With enough memory, BSGS with `±` reaches
`q = 1.0`; by alternating small and giant steps with an orderly arrangement of the tiles, the estimate comes down to `≈ 0.94`
(not verified). The theoretical minimum is unknown: a counting argument gives only `q ≳ 0.67`.

*Counting argument (the author's own, in the spirit of Shoup).* In the generic model every element obtained has exponent `a + b·x`
with `x` unknown. The only information about `x` comes from two elements that coincide (or are opposite): a pair
with `b₁ ≠ ±b₂` fixes at most 2 values of `x`, a pair with `|b₁| = |b₂|` at most 1, two elements with `b = 0` (tame) none.
With `n` elements there are at most `n²/2` pairs × 2 values, so the probability of having found `x` after `n` operations is
`≤ n²/W`, and therefore `E[cost] ≥ ∫₀^{√W} (1 − n²/W) dn = (2/3)·√W ≈ 0.67·√W`. It is a bound that errs on the side of optimism (pairs with
`|b₁ − b₂| > 1` almost never give an integer `x` in the interval), not a construction: no known algorithm reaches it.

**Where it is used.** BSGS with `q = 1.0` needs `T = √W/2` entries (≈ 12 bytes each): for puzzle 135 that would be `2^66` entries,
out of reach for any machine, and for the open puzzles with a known public key (140–160) `2^69…2^79`. The gain (≈ 20 % of time compared
with the kangaroo, plus a *deterministic* cost without the long tail of the kangaroo) exists for `W ≲ 2^68` with 128 GB of RAM and is
implemented in 1.1.0 (section 9). With little memory the best known minimum remains ≈ 1.2–1.3.

### 8.2 The exponent

* **A single instance, "black box" group: `Ω(√W)`** (Nechaev, Shoup). The count above shows it for intervals:
  no algorithm that uses only the group operation, however complex, can go below `√W`. Only the constants change.
* **Exploiting the structure of secp256k1**: for the ECDLP over prime fields no sub-exponential algorithm is known (index calculus, e.g.
  with Semaev's summation polynomials, is not competitive). The symmetries of the curve (`±P`, and the endomorphism `λ` of order 3
  because `j = 0`) give constant factors: up to `√6` on the whole group, but on short intervals only `±P` remains useful (the author's
  reasoning: `λ·e mod n` does not fall in the interval).
* **Many instances with the same width (precomputation)**: Bernstein–Lange bring the cost per instance to `≈ 1.93·W^{1/3}`
  multiplications after a precomputation of `≈ 1.21·W^{2/3}` and a table of `W^{1/3}` entries (experimental measurements); in the generic
  model the tradeoff `S·T² = Ω̃(ε·N)` (Corrigan-Gibbs–Kogan) is tight. Precomputation pays off only with `≳ W^{1/6}` instances
  of the same width (≈ `2^22` for puzzle 135): not our case (few instances, different widths).
* **Several simultaneous targets**, same width `W`, `m` instances: with a shared tame table the total cost grows as
  `√(m·W)`, i.e. `√(W/m)` per instance (counting the products `n_T·n_W` as above). For the open puzzles, of very different widths, the gain is small.
* **Without a public key** the problem is the inversion of `hash160(k·G)`: classically `Θ(W)` (mean cost `W/2`), no better
  exponent; Hellman-type time–memory tradeoffs cost a precomputation `≥ W`, useful only with many targets.
* **Quantum (theory only, no suitable machines exist today)**: Shor's algorithm solves the ECDLP in time polynomial
  in `log n` (Roetteler–Naehrig–Svore–Lauter: `9n + 2⌈log₂ n⌉ + 10` logical qubits, i.e. 2330 for `n = 256`); for *short* logarithms
  (exponent of a few bits, as in the interval) Ekerå–Håstad reduce the circuit size further. Without a public
  key, Grover would reduce `W/2` to `≈ (π/4)·√W` oracle calls (`q ≈ 0.785`), but each call contains a
  scalar multiplication and two reversible hashes, and the algorithm parallelizes badly.
* **Weaknesses in key generation** would be an attack of a different nature (not a discrete logarithm). The author has
  no verifiable information on this and does not consider it.

### 8.3 Summary

| Goal | Possible? |
|------|-----------|
| `q < 1` with little memory | not known; the best known is ≈ 1.15–1.36 (here ≈ 1.02 with schema 3 at large scale, 8.10) |
| `q ≈ 1.0` with a lot of RAM | yes (BSGS with `±`, implemented in 1.1.0): `W ≲ 2^68` with 128 GB; better than the kangaroo up to `W ≈ 2^70` |
| exponent `< 1/2` for a single instance, classical | no in the generic model; no specific attack on secp256k1 is known |
| exponent `1/3` per instance | yes with a `W^{2/3}` precomputation and many instances of the same width |
| polynomial time | only with a fault-tolerant quantum computer |

Additional sources: Bernstein–Lange, *Computing small discrete logarithms faster* (<https://eprint.iacr.org/2012/458>);
Corrigan-Gibbs–Kogan, *The Discrete-Logarithm Problem with Preprocessing* (<https://eprint.iacr.org/2017/1113>);
Roetteler et al., *Quantum resource estimates for computing elliptic curve discrete logarithms* (<https://eprint.iacr.org/2017/598>);
Ekerå–Håstad, *Quantum algorithms for computing short discrete logarithms and factoring RSA integers* (<https://eprint.iacr.org/2017/077>).

### 8.4 Cost of a step and hardware bottlenecks (1.5.0)

With the algorithm fixed (constant ≈ 1.15 since 1.6.0 and exponent 1/2, sections 8.1–8.2), what remains is the cost of **one step** and how many
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
100 % memory-bound. Hence the v2 (section 5): per step x and y are read and written (128 B), the batch products (64 B), the tags and flags
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

**BSGS.** After the vectorization of the windows (section 9.2) a giant step costs ≈ 25 ns of arithmetic and ≈ 55 ns of random
read of the table (single thread, 64 MB table). For large tables the RAM latency decides: huge pages (TLB) and
prefetch matter; a filter in cache does not help when the table itself is tens of GB, and sorting the probes by
address would give locality only with batches comparable to the table. BSGS in any case does not apply to the open puzzles (8.1).

**Orders of magnitude for puzzle 140** (8.85·10²⁰ steps on average with schema 3, median ≈ 0.83·10²¹): at 1.03 G steps/s (measured on a
laptop with a discrete GPU, an integrated GPU and the CPU) that is ≈ 27,000 years on average; the probability of succeeding in one year is
≈ `(n/c)² ≈ 10⁻⁹` (model of section 11). Every doubling of speed halves the time, but the order of magnitude remains.

### 8.5 Comparison with the state of the art: puzzle 135 and RCKangaroo (1.6.0)

Puzzle 135 (`W = 2^134`) has been solved, reportedly with about 300 GPUs in 5 months (a reported figure, not verified here). Calculations:

* expected steps with `K = 1.15`: `1.15·2^67 ≈ 1.7·10²⁰`;
* 300 RTX 4090 with the fastest public kernel (RCKangaroo: 8 G steps/s declared in the first versions, 14.5 in the latest)
  for 1.3·10⁷ s: `3.1–5.7·10¹⁹` steps, i.e. **18–33 % of the expected effort**;
* in the model of section 11 the probability of finishing within that fraction is ≈ 3–8 %: either it went well, or the resources
  were larger than reported. In any case the numbers add up only with GPUs doing 8–14 G steps/s.

What follows for this program:

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
  with 2 million products (`tools/sass/kara_check.c`) and counted with NVRTC 12.0, `ptxas` and `nvdisasm` for sm_86
  (`tools/sass_count.py`, kernels in `tools/sass/`). The test bench costs 96 load and store instructions, subtracted from the figures:

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

Section 8.1 gave a counting bound of `0.67·√W`, obtained by assuming that **every** pair of elements certifies two values of `x`.
Redone with more care (`tools/bounds.py` repeats the calculations):

* The elements are classes `{E, −E}` with `E = b·x + a` (`b = 0` tame, `b = 1` wild, `b = 2, 3, …` walkers on `2x`, `3x`, …). Two elements
  collide in two ways, `E₁ = E₂` and `E₁ = −E₂`, and give `x = (a₂ − a₁)/(b₁ − b₂)` and `x = −(a₁ + a₂)/(b₁ + b₂)`: each is an integer only with
  probability `1/|difference|`, unless the offsets `a` are chosen so that it is.
* Tame–wild: two values, always integer. Wild–wild: one, integer if the offsets are even (the "even distance" trick of the method of
  section 4.2). Tame–tame: none. With coefficients 2 or more the pairs are worth on average `1/2`, `1 + 1/3`, … and **no congruence makes
  them all integer**: a tame walker with even offsets would only see even `x`.
* With tame fraction `α` and the rest wild: coverage `c·n²/W` with `c = 2α(1 − α) + (1 − α)²/2`, maximal for `α = 1/3` with `c = 2/3`: **the
  fraction the program has used since 1.6.0** (not a chance choice: it is the exact optimum). Adding species with coefficient 2…5 the numerical optimum
  stays `(1/3, 2/3, 0, …)`; without the even-distance trick it drops to `c = 0.572` with a 5 % share of `2x` species (consistent with the
  test of 4.6: the third species cost 6 % more).
* Two ways of getting the coverage. **Structured** (sets without overlaps, as in BSGS): `E = (2/3)/√c`, i.e. **`0.8165·√W`** for
  `c = 2/3` (0.882 without the trick). **Random** (independent pairs, Poisson): `E = (√π/2)/√c`, i.e. **`1.0854·√W`** (with `c = 1/2`, tame–wild
  only, it gives `√(π/2) = 1.2533`, the value of the classic kangaroo with `±`).
* The `0.67` of 8.1 is the case in which every pair of different species is always worth 2, with infinitely many species: `c → 1`. With integer values that
  situation does not exist, and the optimum of the model is `0.8165`. It is not a proven lower bound for every algorithm (it remains `Ω(√W)` with unknown
  constant), but it is the best that this family of constructions (tame and wild `±1`, also with more species) can give.

**Where we are.** `tools/ksim.c` (abstract model, `rck=1 tf=0.3333 jabs=12`, `W = 2^34`, 1500 trials per point, error ≈ ±0.02; the cost of
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
points, so the coverage is random and the value to beat is 1.085. Where memory suffices (`W ≲ 2^68`) there is BSGS (section 9): table
first, giant steps afterwards, cost `1.0·√W` with `√W/2` entries. New calculation: **interleaving** the two phases, also keeping the giant points, brings
the cost to `0.9428·√W` (in the pair model with `±`) but only with total memory `M ≳ 1.41·√W`; with `M = √W` it is 0.958, with `M ≈ 0.7·√W` it is worse than plain
BSGS (1.046 against 1.000). The maximum gain is therefore 5.7 %, provided there is more than double the memory of the optimal BSGS;
not implemented: the `bsgs` mode is unusable for the open puzzles anyway.

**What remains.** Between 1.15 and 1.085 there is at most 6 % to recover with random walks (fewer useless merges, better distinguished points).
The statement "no congruence makes all the pairs integer" holds pair by pair, but it does not prevent choosing *for which* `x` each
pair is integer: section 8.8 does this and goes down to 1.045 (schema 2, from 1.7.8). The absolute limit for any generic algorithm is `Ω(√W)`.


### 8.8 Below 1.085: species with fixed residues (schema 2, from 1.7.8)

In 8.7 a pair of walkers with coefficients `b ≠ b'` gave an integer `x` only with probability `1/|b − b'|`, and no congruence
made all the pairs integer together. True pair by pair; but one can **choose for which `x` each pair is integer** and arrange that,
for every `x`, there are enough of them. This is the scheme of the new campaigns from 1.7.8.

* Each walker has `E = b·x + u` with `u ≡ r (mod 12)` and **all jumps multiples of 12**: the residue `r` never changes (the change of
  representative `E → −E` maps `(b, r)` to `(−b, −r)`, i.e. to the same species seen from the other side). The start `u` is uniform in the
  zone of the species and rounded down to the residue.
* Two species `(b, r)` and `(b', r')` can fall on the same class only if `r + (b − b')·x ≡ r' (mod 12)` (or the analogous condition
  for `E = −E'`). When the condition holds, the exponents of the two groups lie on the same lattice of step 12 and meet 12 times
  more often; when it does not hold they never meet. The factor of the pair goes from `1/|b − b'|` to `12/|b − b'|` on the good residues of `x`
  and to 0 on the others: with the right species every residue of `x` receives more useful pairs than before.
* `tools/kspecies.py` computes the exact coverage (geometry of the zones, the two ways of colliding, the Jensen penalty) and searches for the species:
  first it enumerates the assignments of residue classes to species, then it optimizes the shares. The best one for `b ∈ {0, 1, 3}` (tame walkers,
  wild on `Q`, wild on `3Q`):

| species | residues mod 12 | zone of `u` (in units of `W`) | share |
|---|---|---|---|
| tame, `E = u` | 0, 3, 6, 9 (shares 4.2 %, 9.4 %, 4.2 %, 11.6 %) | `[−0.023, 0.023]` | 29.4 % |
| wild on `Q`, `E = x + u` | 3, 7, 11 | `[−1.005, 0.005]` | 34.9 % |
| wild on `3Q`, `E = 3x + u` | 9 | `[−3.005, 0.005]` | 35.8 % |

  Cost in the random-coverage model: **1.045·√W**, against 1.085 for the optimum without residues (the 1.6.0 layout). Other families
  (`b ∈ {0, 1, 3, 5}` mod 60, `{0, 1, 3, 9}` mod 36) give the same value in the ideal model without geometry (0.991); coefficients 2, 5, 9
  do not help. The value 0.991 is the floor of this idea; the zones overlapping non-uniformly bring it to 1.045.
* **Walk model** (`tools/ksim.c design=tools/kang_schema2.txt jabs=12`, jumps in `[2/3, 4/3]` of `12·√W` rounded to multiples of
  12; `W = 2^36`, `dp = 0`, 5000 trials per point, error ≈ ±0.008):

| walkers `K` | schema 1 (1.6.0–1.7.7) | schema 2 | difference |
|---|---|---|---|
| 256 | 1.127 | 1.091 | −3.2 % |
| 1024 | 1.127 | 1.053 | −6.6 % |
| 4096 | 1.092 | **1.044** | −4.4 % |

  With many walkers and a negligible delay of the distinguished points (the regime of the open puzzles, 8.7, *but see the correction in 8.10*) schema 2 reaches its
  theoretical value 1.045 just as schema 1 reached its 1.085–1.09: **about 4 % fewer steps**. With few walkers and dense distinguished points
  the advantage shrinks (`K = 256`, `dp = 2`, `W = 2^32`: 1.127 for schema 2 against 1.161 for schema 1) and with 64 walkers it reverses (`W = 2^28`, `dp = 2`: 1.276
  against 1.214): with few walkers per species the coverage of each residue of `x` is too irregular. For this reason a new campaign uses
  schema 2 only with at least 256 walkers (always, on the open puzzles).
* **On the real curve** (`pzsolve kstat`, 1 thread, hence `K ≈ √W/256` and automatic `dp`; `PZ_KANG_SCHEMA=1|2` chooses the schema):

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

* **Cost per step: none.** The CPU and GPU step is the same; only the jump table (computed once), the starts
  (`u·G + b·Q'`, with `3·Q'` computed once) and the solution change: a pair with type codes `k₁, k₂` gives `(c₁ − c₂)·x = d₂ − d₁` with
  `c ∈ {0, +1, −1, +3, −3}`, exact division by `|c₁ − c₂| ∈ {1, 2, 3, 4, 6}`. The step still writes the usual 2-bit code (tame or
  wild with its sign) and the host turns it into the 3-bit code of the species, so the GPU kernel did not change. The abscissa label in the table
  goes from 62 to 61 bits (false positives are verified anyway).
* **Compatibility.** The schema is decided when the campaign is created: a `dp.bin` in format 1 or 2 (campaigns of 1.7.7 and earlier) keeps it
  on schema 1, which stays identical; a new campaign uses schema 2 and log format 3 (`"PZDP3"`, base = start,
  fingerprint with `"PZDPLOG3"`). The points of the two schemas do not meet: `merge` refuses a log of the other schema and explains why, and to
  add a computer to a schema 1 campaign it is started with `PZ_KANG_SCHEMA=1`.

**What remains.** Between 1.045 and the structured coverage 0.8165 there is still 22 %, which without memory `∝ √W` does not seem recoverable with
random walks; the floor of the residue idea (0.991, without geometry losses) indicates that at most 5 % would be left to gain from better zones.


### 8.9 More RAM or a precalculation: what can be bought (analysis, after 1.7.8)

Question: with a more complex algorithm, more RAM or a precomputation kept in memory, can the factor in front of √W be lowered by a lot?
Short answer: **for a single key, no**. Memory lowers the factor only when it is of the order of √W, and a precomputation makes the
later search fast only by moving the cost, increased, into the precomputation itself.

**1. The limit for one key.** In the generic model every algorithm, with any amount of memory, must compute `Ω(√W)` elements
(Shoup, Nechaev; 8.1–8.2). Memory can change only the constant:

| method | mean factor | memory |
|---|---|---|
| kangaroo, schema 3 (8.10) | ≈ 1.02 (model, open puzzles) | the table of distinguished points: negligible |
| BSGS with `±`, `T = √W/2` | 1.0 | `√W/2` entries |
| interleaved BSGS (8.7) | 0.943 | `≳ 1.41·√W` entries |
| structured coverage, tame and wild `±1` (8.7) | 0.8165 | `∝ √W` |
| pair counting (8.1) | ≥ 0.67 | any |

Even with unlimited memory the maximum known gain is therefore about 10 to 20 %, and it requires `√W` entries. For puzzle 135 that is `2^66` entries, i.e.
`≈ 2·10^21` bytes at 32 bytes per entry. With memory `M ≪ √W` the gain is of the order of `M/√W` (the author's reasoning: a table of `M` fixed points
adds `n·M` useful pairs to the `n²` of the walks). With 64 GiB, `M ≈ 2^31` and `n ≈ 2^67`: `2^−36`, i.e. nothing.

**2. Precomputation (Bernstein–Lange).** Many "tame" walks that do not depend on the key are started, each up to a distinguished point, and a table
keeps the `T` distinguished points reached by the largest number of walks. Then, for a key, it suffices to walk from
`Q + r·G` to a distinguished point and hope that it is in the table. The table is valid for **every** interval of the same width (just translate the
public key). Measurements in the abstract model (`tools/blsim.c`, kangaroo without `±` classes, 200–400 keys per row):

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
do not help it. The same holds for multiple targets without a table (8.2): with widths so different the gain is negligible.

**4. Other ways of using RAM to go faster.**
* *Cheaper step*: every new point requires at least one addition on the curve. Tables help only the starting points (the comb of
  `ec_mul_g`, already in use) and the sharing of the inversion (already done over 4096 walkers per CPU thread and over those of a GPU work-item, section 8.4).
* *More memory for distinguished points*: it lowers `dp_bits` and so the delay with which a collision is recognized. At real scales that delay
  is already worth ≈ 0.1 % (8.7).
* *"Free" points*: with a single inversion one obtains both `P + J` and `P − J`, as in the giant steps of BSGS (section 9). In a walk the
  second point lies on no track: it is useful only if it is itself a distinguished point, i.e. once in `2^dp_bits`. Not worthwhile.

**Conclusion.** With the resources of one computer (or of many) the factor that matters is that of the kangaroo, ≈ 1.02 at large scale with schema 3 (8.10). RAM does not
lower it appreciably and precomputation raises it. The levers left are those of the step (speed per step, sections 8.4–8.6) and, for a few
percentage points, the geometry of the species (8.8).


### 8.10 Local walks, long jumps and banded zones (schema 3, from 1.7.9)

**The defect found.** With `±` classes the walk has no drift: every step moves `|E|` by `±s` (s = jump), so after `n` steps a
walker has explored only a region of width ≈ `√n·s` around its start. The models of 8.7 and 8.8 were measured with short walks
(the simulator with `dp = 0` restarted a walker at every cycle of 2 steps, i.e. about every 500 steps), where this does not show. With
long walks two walkers can meet only if their regions overlap, and the number of overlapping tame–wild pairs is about

`P ≈ (s / W^¾) · K^1.5`, with `K` walkers and `n ≈ √W/K` steps each.

With small `P` the coverage depends on a few pairs, which meet many times, wasting the extra meetings. For puzzle 135
(`W = 2^134`) with one laptop GPU and the CPU (`K ≈ 2^19.4`) and the jump of `12·√W` of schemas 1 and 2: **`P ≈ 0.6`**. RCKangaroo does not have the problem
because it runs with tens of millions of walkers on many GPUs (`P` grows as `K^1.5`).

**Measurements.** Simulator (`tools/ksim.c`, which now counts merges and restarts; `rsmin=N` restarts a walker at the first distinguished point
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

**Schema 3.**
* **Mean jump `W^¾/16`** (a multiple of 12 as in schema 2): `P ≈ K^1.5/16`, i.e. 256 with 256 walkers and ≈ 2^25 on puzzle 135, while the
  explored region is `W/(16·√K)`, a fraction `1/(16·√K)` of `W`, much smaller than the start zones. On the test intervals (`W ≤ 2^44`) the jump
  goes from 16·√W (at `2^32`) to 128·√W (at `2^44`), longer than the 12·√W of before.
* **Banded zones**: the same species and the same residues as schema 2, with every zone split into bands (tame: 2 bands of `0.02·W`; wild on `Q`: bands of
  `0.1·W`; on `3Q`: bands of `0.2·W`) and a share per band, optimized in the exact coverage model (`tools/kbands.py`; in the
  program `src/kang_spec3.inc`, generated from `tools/kang_schema3.txt` with `tools/kspec_table.py`). The density of the wild walkers on `Q` concentrates
  near the two ends of the zone, that of those on `3Q` in the middle. Coverage model: 1.0455 to 1.0344 (1.0304 with finer bands, not adopted: in the
  walks no difference). Walk model, `W = 2^36`, 4096 walkers, `dp = 0`, 5000 trials, two seeds: **1.015 and 1.021 against 1.044 and
  1.053** for schema 2. With 64 walkers the bands do not help (1.063 against 1.054).
* Each walker has its group fixed (coefficient and residue, as in schema 2) and at every start draws the band according to the shares: with
  51 bands and few walkers, a fixed band per walker left some residues uncovered and certain keys without a solution (found
  by the self-test on `W = 2^24` with 32 walkers).
* A fourth species does not help: in the ideal model, with `{0, 1, 2, 3}` mod 60 and `{0, 1, 3, 5}` mod 120, the optimizer gives them zero share and stays at
  0.9908, the value of three species. The theoretical calculation with four species (0.957) requires a uniform distribution of the residues that those families do not allow.

**On the real curve** (`kstat`, `K ≈ √W/256`, where `P` is already large even with the old jumps: here only the effect of the bands counts):

| W | walkers | trials | schema 1 | schema 2 | schema 3 |
|---|---|---|---|---|---|
| 2^32 | 256 | 3000 | 1.151 | 1.134 | **1.105** |
| 2^36 | 1024 | 2000 | 1.170 | 1.095 | **1.056** |
| 2^40 | 4096 | 600 | 1.146 | 1.099 | **1.026** |
| 2^44 | 16384 (8192 for schema 3: 2 threads) | 400 | 1.188 | 1.071 | **1.051** |

  Mean from `2^32` to `2^44`: **1.060** against 1.100 (schema 2) and 1.164 (schema 1); it is the constant of the time estimates (`KANG_C_MEAN`). At the scales of the
  open puzzles the model gives ≈ 1.02.

**Compatibility.** Schema 3 writes the log in the `PZDP4` format (fingerprint `PZDPLOG4`). The campaigns of schemas 1 and 2
stay as they are, because changing the jumps would make the points already found useless. If their `P` is below 64 the program says so at startup and
recommends a new campaign: the work of a few weeks is a negligible fraction of the expected effort on the open puzzles, while schema 3
costs about 25 % less.
## 9. Symmetric BSGS with a RAM table (`--mode bsgs`, from 1.1.0)

### 9.1 Algorithm

Unknown key `k ∈ [lo, hi]`, `W = hi − lo + 1`, target `P = k·G`. Set `c = lo + ⌊(W−1)/2⌋`, `d = k − c`, `|d| ≤ H = ⌈W/2⌉`.

* **Baby steps.** The x-coordinates of `b·G` for `b = 1…T` go into a hash table (`b` and `−b` have the same x-coordinate: one entry
  covers two points).
* **Giant steps.** With `S = 2T + 1`, for `a = −A…A` (`A = ⌈H/S⌉`) the table is searched for the x-coordinate of
  `Q_a = (P − c·G) − a·S·G`. A hit with index `b` means `Q_a = ±b·G`, hence `k = c + a·S ± b` (both signs are tried and
  `k·G = P` is verified).

Every `d ∈ [−H, H]` can be written uniquely as `a·S + b` with `|b| ≤ T` and `|a| ≤ A`: the search is **exhaustive and
deterministic** (no probability of failure as with the kangaroo). With the giants visited in the random order of the seed the mean
cost is `T + W/(4T)` group operations (half of the giants on average), minimum `1.0·√W` with `T = √W/2`; in the worst
case `1.5·√W`. The case `b = 0` (the key falls exactly on a giant step: `Q_a` is the point at infinity) is handled by a slow exact
path, which is also triggered by every zero denominator in the batch inversion.

### 9.2 How it is implemented

* **Giant steps in groups**: as in the linear search, one batch inversion (Montgomery's trick) serves a window of 1024
  giants `C ± i·U` with `U = −S·G`; only the x-coordinates are needed here, so the multiplication for `y` is saved too
  (≈ 2.5M + 1S per step, against the 3.5M + 1S of the linear search).
* **Table**: open addressing with linear probing, **8 bytes per entry** (24-bit tag taken from `x.n[1]`, 40-bit
  index `b`), load factor 0.65 (≈ 12.3 bytes per baby step), cell index `⌊x.n[0]·N/2^64⌋` (any number of
  cells), *lock-free* insertion with `compare_exchange` and **idempotent** (safe to run again), transparent huge pages
  (`madvise(MADV_HUGEPAGE)`) to reduce TLB misses. A tag that matches by chance (probability `~2^-24` per cell
  checked) is a false positive discarded by the `k·G = P` check and counted in the statistics.
* **Parallel construction**: blocks of 2^22 baby steps taken from an atomic counter; each thread uses the same batch-inversion
  window. Measured on 4 cores: ≈ 16–24 M entries/s.
* **SIMD lane windows (1.5.0)**: 4 (AVX2) or 8 (AVX-512, IFMA) consecutive windows per instruction, with the arithmetic of
  `fev.h`/`fev52.h`, both for the giants and for building the table. At startup each variant must produce exactly the
  x-coordinates of the scalar routine (two calls, so the advance of the centers is covered too), otherwise it is not used;
  `PZ_BSGS_SCALAR=1` disables them. Here (4 threads, table 2^22): giants from 25.8 to ≈ 50 M/s; the limit is now the random read
  of the table.
* **Search**: the giants are the "keys" 1…Ng of a scheduler identical to the linear one (same permutation from the seed,
  same checkpoints/resume); software `prefetch` at distance 16 hides the RAM latency.
* **Persistence (`--table FILE`)**: the table depends only on `G` and `T`, **not on the target or the interval**:
  it is built once, saved (4 KiB header with SHA-256, `T`, x-coordinate of `T·G`, checksum of the body; written
  to `.tmp` with the header last, then `rename`) and read back for any later campaign (checks: size, checksum, number of entries,
  256 baby steps recomputed at random). For a fixed `T` the cost of a new key drops to
  `W/(4T)` giants.
* **Automatic selection**: with a known public key and an interval ≥ 2^20 the program chooses `bsgs` if the optimal table (or the
  largest one that fits in 70% of free RAM, or in `--mem`) costs less than the kangaroo (`KANG_C_MEAN`: 1.06·√W from 1.7.9, 1.10 in 1.7.8, 1.15 before) and **there are no GPUs** (or the job
  is a few seconds, or the table already exists on disk); otherwise it uses the kangaroo, which exploits the GPUs. `T` is fixed at
  campaign creation and saved in `campaign.conf` (`bsgs_entries`).
* CPU and RAM only: the GPUs do not take part (the kangaroo remains the choice for using them).

### 9.3 When it pays off

| `W` | optimal `T` | table RAM | BSGS cost | kangaroo (1.15·√W; with schema 2 it is 4% less) |
|-----|-------------|-----------|-----------|--------------------------------------------------|
| 2^50 | 2^24 | 0.19 GiB | 3.4e7 | 3.9e7 |
| 2^60 | 2^29 | 6.2 GiB | 1.1e9 | 1.2e9 |
| 2^64 | 2^31 | 25 GiB | 4.3e9 | 4.9e9 |
| 2^68 | 2^33 | 98 GiB | 1.7e10 | 2.0e10 |
| 2^70 | 2^34 | 197 GiB | 3.4e10 | 4.0e10 |

With 128 GB of RAM (table ≤ 90 GiB) the optimal BSGS fits up to `W ≈ 2^67.7`; with a table smaller than `√W/2` the cost is
`T + W/(4T)`, which stays below the kangaroo for `T ≥ 0.29·√W`, that is up to `W ≈ 2^69` (with the kangaroo constant of 1.26 used
up to 1.5.0 it was `0.24·√W` and `2^70`).

**Honesty about applicability.** The open puzzles with a known public key (135, 140, 145, 150, 155, 160) have `W = 2^134…2^159`:
`T` would be of the order of `√W = 2^67…2^79.5`, unthinkable. For them BSGS cannot help and the kangaroo remains (still ≈ 2^70 steps for 140). BSGS
is useful for intervals with a known public key up to `W ≈ 2^70` and as a "universal solver" with a reusable
precomputed table.

## 10. SIMD lane arithmetic (`avx2v`, `avx512v`, `avx512i`)

The linear search spends about 75% of its time in curve arithmetic (field multiplications) and the rest in hashing.
The scalar 4×64-bit multiplications (MULX/ADX) cost ≈ 37 cycles each on the test machine used below. The `*v` engines instead run the
same arithmetic on **4 (AVX2) or 8 (AVX-512) independent windows** in parallel, one per lane:

* **Representation** (`lib/pzcore/include/pzcore/fev.h`): 9 limbs of 29 bits per element, "structure of arrays" (limb `i` of all lanes is one
  register). `vpmuludq` (32×32→64) does 4 or 8 products per instruction and a column of 9 products of ≤ 2^60 fits in 64 bits.
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

## 11. Time-estimation model (`src/eta.c`)

Estimates are computed from a *realistic rate from the start* (since 1.7.4, `rate_est` in `eta.c`): no estimate until every
engine has produced something (a GPU counts only at the end of a launch), then the average over a sliding window of up to 2 minutes; at the start
it is blended, with a weight growing up to 150 s, with a *remembered* speed (the steady-state speed of a previous run of the same
configuration, which on laptops is lower than the cold speed measured by tuning). BSGS, which changes speed at the end
of table construction, keeps a moving average of ≈ 30 s. They show the time *still needed* and the probability of
success in the next 1 h / 24 h / 7 d / 30 d / 365 d:

* **Linear and BSGS** (the key is uniform over the units not yet checked): `P(within e units) = e / remaining`; time for
  probability `q`: `q·remaining / speed`.
  With `--part` the computer can scan only its own part (`reach` units still to do) while the key is uniform over all
  the `remaining` units it has not yet checked: `P(within e) = min(e, reach) / remaining`, and probabilities `q` beyond `reach/remaining`
  cannot be reached alone (time "n/a").
* **Kangaroo**: cost `n` with `P(found within n) = 1 − exp(−(n/c)²)`, `c = k/0.8862·√W` with `k` the mean cost of the campaign's schema: 1.06 for schema 3 (from 1.7.9, section 8.10, `c ≈ 1.20·√W`), 1.10 for schema 2 (from 1.7.8, section 8.8, `c ≈ 1.24·√W`), 1.15 for schema 1 (from 1.6.0, section 4.5, `c ≈ 1.30·√W`; `c = 1.43·√W` before). Given that after `n₀` steps it has
  not been found yet: `P(within n₀+e | not found) = 1 − exp(−e(2n₀+e)/c²)` and `e_q = c²·L/(n_q + n₀)` with `L = −ln(1−q)`
  (the form without numerical cancellation).

These probabilities are *conditional* on the key being in the interval and on the program being correct; they say nothing
about whether the funds are still there (check the address with a blockchain explorer).
