# pzsolve algorithms

This document explains *what* the program computes and *why* it is efficient. For the structure of the code see
[`ARCHITECTURE.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/ARCHITECTURE.md), for usage see [`USAGE.md`](USAGE.md). What was tried and discarded, the intermediate measurements
and the analyses behind the design are in the research log [`RESEARCH.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RESEARCH.md). The section numbers are the same in both files (they are cited
from the source and from the tools); sections 4.5, 4.6, 8.6, 8.7 and 8.9 are in RESEARCH.md only.

Sections 1 to 7 cover the problem, the linear search, the seed-based visiting order, the interval kangaroo, the GPU kernels, rough running times and references. Section 8 is the theory: what is proven in the literature,
what was measured, what is only modelled, and how much improvement is still possible. Sections 9 to 11 cover the BSGS mode, the SIMD
lane arithmetic and the time-estimation model. Numbers are labeled as measured on the real curve, measured in a model
(`tools/research/ksim.c`), or estimated.

## Contents

1. The problem
2. Linear search (address only): 2.1 Arithmetic, 2.2 Vectorized hash160, 2.3 Edge cases
3. Unique visiting order (seed)
4. The interval kangaroo (public key known): 4.1 Setup, 4.2 Why it costs less, 4.3 Unproductive cycles,
   4.4 Parameters (chosen automatically), 4.7 Resuming
5. GPU (CUDA and OpenCL)
6. How long it takes (orders of magnitude)
7. References
8. Theoretical limits: how much can still be improved? 8.1 The constant in front of sqrt(W), 8.2 The exponent, 8.3 Summary,
   8.4 Cost of a step and hardware bottlenecks, 8.5 Comparison with the state of the art (puzzle 135 and RCKangaroo),
   8.8 Species with fixed residues (schema 2), 8.10 Local walks, long jumps and banded zones (schema 3)
9. Symmetric BSGS with a RAM table (`--mode bsgs`): 9.1 Algorithm, 9.2 How it is implemented, 9.3 When it pays off
10. SIMD lane arithmetic (`avx2v`, `avx512v`, `avx512i`)
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
  key. An affine addition needs one modular inversion, which costs about as much as 70 multiplications (the divsteps of Bernstein and
  Yang, in variable time, `lib/pzcore/src/fe.c`; the addition chain to `p − 2` costs 270): it is amortized with
  **Montgomery's trick** (a single inversion for a whole vector of denominators).
* **Symmetric group.** For a center `C = c·G` and the table `i·G` (i = 1…H), the denominators `x(i·G) − x(C)` serve both
  `C + i·G` and `C − i·G` (same x-coordinate, opposite y): one inversion for `2H` consecutive keys, plus the next
  center `C + 2H·G`. With `H = 256` (CPU) the cost is ≈ `3.5 M + 1 S` per key (M = field multiplication, S = field squaring)
  plus 1/512 of an inversion.
* The hash needs the parity of `y`: it is obtained by also computing `y` (one multiplication) instead of hashing twice.
  Only `x` and that parity have to be canonical. In the lane engines a carry pass and one fold give both, and the full reduction runs
  only when the top limb ends all ones (about one lane in 2^24, every value ≥ p among them; with IFMA every element is canonical already
  below that). For `x3 = λ² − x(C) − x(i·G)` the scalar engines and the GPU subtract the sum `x(C) + x(i·G)`, formed once for both
  points of a pair; the 29-bit lane engines subtract both terms in one pass. With IFMA the subtrahends go into the products: `x3 = λ² +
  (K2 − x(C) − x(i·G))` and `y3 = λ·t + (K1 − y(C))` add the term (a multiple of `p` less the subtrahends, formed once per round) to the
  low columns before the reduction, whose carry pass leaves the result exact; the separate subtraction and its carry pass, a stage of the
  dependency chain of every point, are gone (RESEARCH.md 10.6).
* The lane engines hash the candidates eight batches at a time (8 × 8 or 8 × 16 keys), so that the curve arithmetic of consecutive
  points runs back to back and out-of-order execution overlaps their dependency chains.

### 2.2 Vectorized hash160

SHA-256 (a single block: 33 bytes + padding) and RIPEMD-160 (a single block: 32 bytes + padding) are written once with GCC's
*vector extensions* and instantiated with 4 lanes (SSE2/NEON), 8 lanes (AVX2) and 16 lanes (AVX-512). The engine to use is
chosen at runtime with a 0.25 s micro-benchmark (`--isa` forces it). For each batch only **one word** of the digest is
compared with the target: the third, which depends on the fewest steps of RIPEMD-160 (the compiler drops the last three steps
of the left line and the last four of the right one); the rare candidates are verified in full with the scalar hash and then
verified again by recomputing `k·G` from scratch.

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
  computed in O(1) (a few SHA-256 per block, negligible next to the 2^36 keys of a block). Its inverse lets
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

* **Several computers (`--part`)**. The *positions* of the visiting order are split, not the interval: with `T` shares, the computer
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

The program uses one layout of the walkers, called schema 3 in RESEARCH.md: three species of walkers with fixed residues mod 12
(section 8.8), long jumps of mean `W^¾/16` and start zones split into bands (section 8.10). It costs about 1.06·√W steps on the real curve.
The earlier layouts of the development builds (schemas 1 and 2) and how the program got here are in RESEARCH.md section 4.

### 4.1 Setup

Let `base` be the start of the range and `Q' = Q − base·G`: the key is `k = base + x` with `x ∈ [0, W)`. The point `3·Q'` is computed once.

**Equivalence classes.** `P` and `−P` have the same x-coordinate; we work on the classes `{P, −P}` and choose as
representative the point with even `y`. A walker (kangaroo) carries its canonical point and the exponent of that point written in terms of the
unknown `x`: `E = σ·c·x + d`, where `c` is the coefficient of its species, `σ = ±1` its current sign and `d` an exact signed 192-bit integer.

**Species.** `c = 0` (tame), `c = 1` (wild on `Q'`) or `c = 3` (wild on `3Q'`). A walker starts at `E = c·x + u`, the point `u·G + c·Q'`,
with `u` in the zone of its species and in a fixed residue class mod 12. Every jump is a multiple of 12, so the residue of a walker never
changes (the change of representative `E → −E` maps `(c, r)` to `(−c, −r)`, the same species seen from the other side). The table is
`src/kang_spec3.inc` (shares rounded here):

| species | residues mod 12 (share of the walkers) | zone of `u` (in units of `W`) | bands | share |
|---|---|---|---|---|
| tame, `E = u` | 0, 3, 6, 9 (3.8 %, 10.3 %, 3.8 %, 10.3 %) | `[−0.02, 0.02]` | 2 of `0.02·W` | 28.2 % |
| wild on `Q'`, `E = x + u` | 3, 7, 11 (12.7 % each) | `[−1, 0]`, without `[−0.7, −0.6]` | 9 of `0.1·W` | 38.1 % |
| wild on `3Q'`, `E = 3x + u` | 9 | `[−3, 0]` | 15 of `0.2·W` | 33.7 % |

Each walker belongs to one group (coefficient and residue) for its whole life; the groups are dealt out in proportion to their shares, as
exactly as the number of walkers allows. At every start the walker draws one of the 50 bands of its group at random by the shares and a
point uniform in the band, rounded down to its residue.

**Step.** `j = x(P) mod 256`, `P ← canon(P + S_j)`, `d ← d + s_j`; if `y` is odd the point is negated (`d ← −d`, `σ ← −σ`). The 256 jumps
`s_j` (plus 16 for leaving cycles, 4.3) are pseudo-random from the seed: uniform in `[2/3, 4/3]` of the mean `W^¾/16` and rounded up to a
multiple of 12. The mean is computed with integers only (`⌊√W⌋·⌊W^¼⌋/16`, at least 24, at most 2^120), so the table is the same on every
architecture. Its **jump table id** (the first 4 bytes of the SHA-256 of the 272 jumps, in hex) is written to `log.txt`, and to the screen with
`--node`: all the computers of one campaign must show the same id. Since the representative has a random sign, the exponent does a symmetric
walk (no drift): each walker explores the surroundings of its start, which is why the jumps are long (section 8.10).

**Distinguished points (DP).** When the second 64-bit word of `x(P)` has `dp_bits` low zero bits, the record `(tag, type, d)` goes into the
table: a 61-bit tag (the third word of `x(P)` without its 3 low bits) and a 3-bit type, 0 tame, 1/2 wild on `Q'` with `σ = +1`/`−1`, 3/4 wild
on `3Q'` with `σ = +1`/`−1` (the step writes tame or wild with its sign, the host adds the species). Two records with the **same tag and a
different type** give

`(c₁ − c₂)·x = d₂ − d₁`, with `c ∈ {0, +1, −1, +3, −3}` the signed coefficient of each type,

and the division by `|c₁ − c₂| ∈ {1, 2, 3, 4, 6}` must be exact: the residues of the species make it so for the pairs that can meet
(section 8.8); any other pair is discarded. Two records of the same type give no information (same trajectory) and the walker that
brought the second one starts again at a random point. Every candidate solution is verified (`(base+x)·G` against the public key) before it is
accepted: a false positive of the 61-bit tag cannot produce a wrong result.

### 4.2 Why it costs less

In the classic kangaroo (tame/wild with positive jumps only) the wild must "catch up" with the tame trail:
`≈ 2·√W` steps. With classes the points live in a roughly halved space and collisions are a *birthday problem*.

The wild walkers start on both sides of zero (`x + u` with `u ∈ [−W, 0]`, `3x + u` with `u ∈ [−3W, 0]`), so the classes fold onto themselves:
two wild walkers at symmetric positions collide and give `x`. Each wild walker can meet almost all the other wild ones (a birthday problem that is
*not* bipartite, more efficient), while the tame ones cover the zone near zero, where the folded wild ones are sparser. The residues mod 12
choose which pairs can meet, so that every `x` receives enough pairs with an integer solution (section 8.8), and the bands set how densely each
part of a zone is covered (section 8.10). Measured on the real curve: **1.06·√W** on average from `W = 2^32` to `2^44`; at the scale of the open
puzzles the model gives ≈ 1.04·√W (RESEARCH.md section 8.12).

### 4.3 Unproductive cycles

Walking on the classes `{P, −P}`, a walker can return to a point it already visited without ever meeting a DP: from then
on it repeats the same points forever (an *unproductive* cycle). With `R` jumps in the table (Bos–Kleinjung–Lenstra, *On the use
of the negation map in the Pollard rho method*, 2010):

* **Cycles of length 2** (`P → P' → P`): probability `1/(2R)` per step. When the current point coincides with the one from
  two steps earlier (32-bit tags), the next jump `(j+1) mod R` is used: the rule depends only on the history `(P_{n−1}, P_n)`, so two
  walkers that merge keep merging.
* **Cycles of length 4, 6, …**: even after removing those of length 2 they remain, with probability `≈ (R−1)/(4R³) ≈ 1/(4R²)` per step,
  that is **one every ≈ 2.6·10⁵ steps** with `R = 256` (measured: 3·10⁵ in the model of `tools/research/ksim.c`, almost all of length 4).
  With `dp_bits = 45` (puzzle 140) a walker meets a DP every 3.5·10¹³ steps: without an exit every walker would be trapped after a few minutes.
* **Detection and exit**: every `64` steps (the same step numbers for the whole herd) the walker stores the 32-bit tag of the current point
  (*checkpoint*) and from then on keeps the minimum tag seen. If it returns to the checkpoint it is in a cycle, and
  the minimum is that of the whole cycle; it keeps going until it reaches the point with the minimum tag and from there makes an **escape
  jump**, taken from a separate table of 16 jumps (same distribution as the normal ones). The exit point depends only on the
  cycle, not on where or when the walker entered it: two walkers that had merged leave in the same way and
  stay merged, so the collision is seen at the next DP. The escape jump is not part of the normal table, so the walk cannot fall into the
  same cycle again with a combination of normal jumps. If the cycle does not close within one period (length ≥ 32: negligible probability)
  detection starts over. Cost: a few comparisons per step and on average ≈ 40 wasted steps per cycle (0.015%). Checks: in the model, with 16
  jumps and no other defence, every search terminates (cost 2.0·√W, against 38% of the searches stuck without the exit); on the real curve the
  self-test solves an interval of 2^30 keys with only 8 jumps (one cycle every ~250 steps), a DP every 128 steps and no restarts, which
  without the exit does not terminate.
* **Last defence**: a walker that has gone `24·2^dp_bits + 16` steps without a DP (a 64-bit counter, tested at the checkpoint steps) is
  started again at a random point.
* **No drift to tune**: the walk on the classes is symmetric; only the length of the jumps matters (section 8.10).

### 4.4 Parameters (chosen automatically)

Here `K` is the total number of walkers and `N` the number of entries that the memory limit holds.

| Parameter | Value | Reason |
|-----------|-------|--------|
| base | the start of the range | `x = key − base` in `[0, W)` |
| species, zones | the table of 4.1 (`src/kang_spec3.inc`) | optimized in the exact coverage model (sections 8.8 and 8.10) |
| jumps | 256 multiples of 12, uniform in `[2/3, 4/3]` of `W^¾/16` (+16 for leaving cycles) | long walks without drift (section 8.10) |
| walkers per CPU thread | `clamp(√W/(256·threads), 16, 4096)`, multiple of 16 | the batch inversion is shared by the whole herd of the thread; 4096 is the measured optimum (RESEARCH.md sections 8.4 and 10.3) |
| GPU walkers | `NK` per work-item (tuned, section 5) × work-items (by default 512 per SM with CUDA, 64 per compute unit with OpenCL, at most 32768) | at most `√W/256` in total, so each one walks long enough |
| `dp_bits` | `max(⌊log2(0.005·√W/K)⌋, ⌈log2(1.3·√W/(0.8·N))⌉, 2)`, at most 60 | the delay of the last DP (`≈ 0.95·K·2^dp_bits` steps, RESEARCH.md 8.13) between 0.25 and 0.5 % of `√W`, 0.35 % on average (0.7 % with the 0.01 of the versions up to 1.15.1, for half the table and half the `dp.bin`); the second term is the memory, below |
| `--dp-bits B` | at most `⌊log2(√W/K)⌋ − 1` (a larger value is lowered, with a warning), never below what memory requires | a walker reaches its next DP after `2^dp_bits` steps: `K·2^dp_bits` must stay well below `√W` |
| DP table | 16 bytes per entry (the tag and the index of the record in `dp.bin`), open addressing; limit `--mem` (default 40 % of the free RAM, at least 256 MiB and at most 64 GiB) | see below |

The table must hold the DPs of `1.3·√W` steps at 80 % load when memory decides `dp_bits` (`KANG_DP_MEM_MARGIN = 1.3`): the search costs
1.06·√W on average and goes beyond 1.3·√W about once in three. It starts with room for those points at 60 % load (within the limit) and
doubles at 80 % load if the old and the new table fit in the limit together. When it cannot grow it is **thinned in place** at 93 % load:
from then on only DPs whose tag has one more zero bit are kept, and the stored ones that do not have it are dropped in one sweep over the array,
with no second table. The search goes on, and a collision is seen with the delay of `dp_bits + 1`, the one a margin twice as large would have had
from the start. Each bit less halves the delay of the distinguished points (about 1 to 2 `K·2^dp_bits/√W` steps, RESEARCH.md section 8.7).

An entry holds the 64-bit `tagk` (61 bits of the abscissa and the 3-bit kind) and the index of the record in `dp.bin`, not the 24-byte distance:
the distance is needed only when two points of different kinds share a tag, about once in a campaign plus a few false matches of the 61 bits.
It is then read back from the buffer of the log, from the block being written, or from the file (under the table lock, then the lock of the log).
A point that is in no log (no file, or a disk that refused it beyond the 2^22 buffered records) keeps a copy of its record beside the table; the
copies count against `--mem` and are thinned with the table. Half the 32 bytes of a whole record means one bit less where memory sets `dp_bits`:
in the model of 1.12.0, with 2^22 walkers, puzzle 140 costs 1.08·√W instead of 1.10 with 32 GB of RAM, 1.10 instead of 1.15 with 16 GB and
1.15 instead of 1.23 with 8 GB; with 2^20 walkers or 64 GB and more the gain is 1 % or less. Measured on 2^40 keys with 4096 walkers and a
`--mem` that sets `dp_bits` (400 keys): 1.27·√W with 32-byte entries, 1.15 with 16-byte ones; at equal `dp_bits` the walks are identical.

### 4.7 Resuming

Every DP that enters the table is appended to `dp.bin`: a header of 42 bytes (the magic `"PZDP5\n"`, a SHA-256 fingerprint of `base`, `W`,
the text `"PZDPLOG4"` and the public key, and the 4 bytes of the jump table id), then records of 32 bytes `{tagk, d}` (the table refers to them by their index). The records are
collected under the lock of the table in a buffer of 32768 (1 MiB); a full buffer is exchanged for an empty one and written after the table lock
is released, under a lock of its own, so a slow disk does not stall the search threads. If the disk refuses a write the buffer grows (up to
2^22 records) and the write is tried again; a record cut short at the end of the file is cut off at the next start.

On resume the table is rebuilt from the file and the walkers restart from new points (a different random stream for each session and each
`--node`); the old trails remain useful because each of their later DPs is already in the table. A file of another campaign (another range or
public key) is refused, and so is one written with another jump table (another seed, or the setup of another computer): the message gives both ids.
A file of the format `"PZDP4"` (the same records, a header of 38 bytes without the jump table id: versions 1.9.1 to 1.11.0) is loaded, appended
to and merged as it is. The formats `"PZDP1"` to `"PZDP3"` of the older layouts cannot be used: `hunt` keeps such a `dp.bin` as `dp.bin.vN` and
goes on with a new file, `merge` refuses it. A file written by a later version is left as it is.

## 5. GPU (CUDA and OpenCL)

* The kernels are written **once** in OpenCL C 1.2 with a field of 8 limbs of 32 bits. For OpenCL the driver compiles them from the embedded
  sources; for CUDA `tools/gen_ptx.py` compiles them with NVRTC (through `src/kernels/pz_cuda.h`, which translates the dialect) into PTX for
  `compute_52`, embedded in the executable and compiled by the NVIDIA driver for the installed card (result cached). Both runtimes
  are loaded dynamically (`dlopen` / `LoadLibrary`): no build-time dependency and no SDK at runtime.
* With CUDA: 128 threads per block (`__launch_bounds__(128)`), grid rounded up and extra work-items that exit immediately
  (each kernel receives the number of work-items as its last argument); hash160 and modular inversion are compiled once as real
  functions (smaller PTX and fewer registers); the context waits
  for the kernels to finish without busy-waiting, so the core is left to the CPU compute threads.
* **CUDA arithmetic**: carry chains in PTX for the addition, the subtraction and the reduction, and a dedicated square; the products are chains
  of `mad.lo.cc`/`madc.hi.cc` pairs that `ptxas` fuses into one `IMAD.WIDE.U32` per limb product, in two accumulators (the products at even
  and at odd positions) so that every addend is an aligned register pair. **OpenCL arithmetic**: the same carry chains with clang's
  add-with-carry builtins on AMD GPUs (and pocl), a branch-free C equivalent elsewhere; on AMD GPUs from gfx9 to gfx12 the product is
  in inline assembly, `v_mad_u64_u32` with its carry out counted by one add-with-carry, on pairs at even and odd positions as on CUDA
  (without it, `-DPZ_NO_ASM`, when the driver refuses it or the device fails the self-test with it). Subtractions of a normalized value
  (the walker's x and y, the jump points, the centre and the table of the linear search) skip the second correction. In the CUDA linear kernel the
  additions of SHA-256 and RIPEMD-160, the carries out of the field addition chains and the normalization of x are `IMAD` with a
  multiplier of 1 read from the constant bank: they run on the FMA pipe and leave the ALU pipe, which bounds that kernel, to the
  rotations, the logic and the carry chains (RESEARCH.md section 5, after 1.17.0; the kangaroo kernel keeps the forms of 1.17.0). **Inversion**: Bernstein–Yang divsteps, 30 per
  round, done in variable time (runs of zero bits at once, up to three bits cancelled per swap). The instruction counts and
  the measurements, made on the sm_86 (Ampere) code and on the reference laptop GPU (20 SM, 4 GB, 192 GB/s, 2 MiB of L2; the card on which the GPU speeds
  in this document were measured): RESEARCH.md section 5.
* **Linear kernel with "fused" inversion**: the backward pass of Montgomery's trick derives each inverse and uses it
  immediately for the points `C ± i·G`, recomputing the denominator from the table instead of keeping it in an array: about 8.7 KB of
  private memory per work-item with a window of 256 and ≈ 32 bytes of traffic to video memory per key. The hash filter compares word 2 of the
  hash160, which does not depend on the last three steps of the left RIPEMD-160 line nor on the last four of the right one (the compiler
  eliminates them); the rare keys that pass are hashed again in full on the device, so a reported hit matches all 160 bits (and is re-verified
  on the CPU anyway). Both points of a pair share `x(C) + x(i·G)`, so `x3 = λ² − (x(C) + x(i·G))` costs one subtraction.
* **Linear**: each work-item scans `ngroups` consecutive groups from its own center `P0 + gid·D` (computed on the
  device with a small double-and-add multiplication); groups of `2·HALF` keys, table `i·G` in constant memory, complete hash
  160 on the device, hits collected with an atomic counter. The duration of a launch is regulated to ~0.35 s.
  `HALF` (the window) is a compile-time constant of the kernel. With CUDA there are six variants: window 128, 256 or 512 × free registers
  (≈ 170) or limited to 128 (`__launch_bounds__(128, 4)`: more resident warps, and a grid of 4 blocks per SM to fill them); with OpenCL four: window 64, 128, 256 or 512. On the first
  run on a device the program measures them all (a few seconds each) and keeps the default (256 with CUDA, 128 with OpenCL) unless another one
  is more than 3% faster; the choice is stored in `gpu_tune.txt` in the cache folder (key: name, driver version, compute units, program version).
  `PZ_GPU_HALF=N` forces it, `PZ_GPU_TUNE=0` disables tuning; `pzsolve selftest` verifies all the variants.
* **Kangaroo**: `NK` walkers per work-item, so that one inversion (by divstep) serves `NK` steps, with the same "fused" scheme as the linear
  kernel (no array of denominators), **128 registers without spill** and therefore 512 resident work-items per SM. The state of the walkers is in
  global memory (x, y, the running products of the batch, 32-bit tags and flags: ≈ 130 bytes per walker). The distance does not travel from step
  to step: every step adds 16 bits (jump index, "negated" bit) to a history buffer, and `d` is rebuilt by replaying it, at the end of a launch of
  at most 64 to 256 rounds (as many as the device memory allows: the history takes at most an eighth of it) for everyone and on the spot for the rare distinguished points. One pass per round moves the walkers and computes the
  denominators of the next round. The DPs go out in a buffer and are inserted in the same table as the CPU's; restarts are handled by the
  host. Variants, measured on the device by the same procedure as the linear ones (the default stays unless another is more than 3% faster):
  with CUDA `NK` = 64, 128 or 256 (default 128), each plain or with the running products kept only for every other walker (`b`, less memory
  traffic for half a multiplication more per step), and `NK` = 64 or 128 with the products every fourth walker (`c`), and on cards of more than
  64 SMs, where the default herd is capped at 32768 work-items, `NK` = 64 at 512 work-items per SM ("full herd"); with OpenCL `NK` = 32, 64
  or 128 (default 64), plain or `b`, and `NK` = 32 or 64, plain or `b`, with 4 or 8 times the 64 work-items per compute unit ("4x herd", "8x
  herd"; at most 128 per compute unit on Intel GPUs, whose compute unit is one EU). On cards with at least 8 MiB of L2 the tuning also tries herds sized to fit in half of the L2 (fewer
  work-items, `NK` 16, 32 or 64, plain or `b`). `PZ_GPU_NK` forces a variant. The design of the kernel around memory traffic and its
  measurements: RESEARCH.md sections 5 and 8.4.
* **Verification**: at startup every device runs (a) field operations, hash160 and point additions compared with the CPU,
  (b) a linear search with a planted key, (c) for the kangaroo **the same group of walkers run on GPU and on CPU,
  with a bit-by-bit comparison of states, DPs and restarts** (with a low step limit, so that restarts really happen). A device
  that fails is excluded; an NVIDIA card that fails through CUDA is retried through OpenCL, an AMD GPU that fails with the products in
  inline assembly is retried without them. Every key found is
  re-verified on the CPU anyway.
* Performance: measured on the reference laptop GPU (RESEARCH.md section 8.4 has the figures); run `bench` to measure your own cards. Functional verification: OpenCL on pocl (OpenCL on CPU); CUDA with a simulated driver that
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

Kangaroo, **mean case** (≈ 1.06·√W steps, `KANG_C_MEAN`, section 8.10):

| puzzle | steps (mean) | 100 M/s (CPU) | 1 G/s | 10 G/s |
|--------|--------------|---------------|-------|--------|
| 60 | 8.05e+08 | 8.05 s | 805 ms | 80.5 ms |
| 70 | 2.58e+10 | 258 s | 25.8 s | 2.58 s |
| 80 | 8.24e+11 | 2.29 hours | 824 s | 82.4 s |
| 90 | 2.64e+13 | 3.05 days | 7.33 hours | 2.64e+03 s |
| 100 | 8.44e+14 | 97.7 days | 9.77 days | 23.4 hours |
| 110 | 2.70e+16 | 8.56 years | 313 days | 31.3 days |
| 120 | 8.64e+17 | 274 years | 27.4 years | 2.74 years |
| 130 | 2.77e+19 | 8.8e+03 years | 876 years | 87.6 years |
| 135 | 1.56e+20 | 5.0e+04 years | 5.0e+03 years | 496 years |
| 140 | 8.85e+20 | 2.8e+05 years | 2.8e+04 years | 2.8e+03 years |
| 150 | 2.83e+22 | 9.0e+06 years | 9.0e+05 years | 9.0e+04 years |
| 160 | 9.06e+23 | 2.9e+08 years | 2.9e+07 years | 2.9e+06 years |

Puzzles without a public key are out of reach for the hardware of a single computer from `n ≳ 70`; those with a known public
key from `n ≳ 110–120`. The program stays correct for any `n ≤ 189` (kangaroo) / `n ≤ 256` (linear),
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
* The abstract simulator used to tune the parameters is in `tools/research/ksim.c`.

## 8. Theoretical limits: how much can still be improved?

This section separates what is **known in the literature** from what is **the author's own reasoning** (marked as such).

### 8.1 The constant in front of √W (constant `q`)

| Algorithm | Memory | Mean cost in units of √W | Origin |
|-----------|--------|--------------------------|--------|
| classic kangaroo (van Oorschot–Wiener) | small | 2 | literature |
| 4 kangaroos / improved Gaudry–Schost | small | 1.715 / 1.661 | Galbraith–Pollard–Ruprai |
| Gaudry–Schost with `±` classes | small | ≈ 1.36 (theoretical) | Galbraith–Ruprai |
| **this implementation** | small (DP table) | **1.06 measured from 2^32 to 2^44, ≈ 1.04 on the open puzzles (model, RESEARCH.md 8.12)**; the earlier layouts: RESEARCH.md section 8.1 | `pzsolve kstat`, `ksim` |
| BSGS with `±`, `T = √W/2` | `≈ 2.9·√W` bytes | **1.0** (worst case 1.5) | direct calculation (RESEARCH.md section 4.6) |
| counting lower bound | any | **≥ ≈ 0.67**, optimistic: with integer values the optimum of the model is 0.8165 (RESEARCH.md 8.7) | the author's own reasoning, below |

**Is `q < 1` possible in theory? Only beyond a certain point.** With enough memory, BSGS with `±` reaches
`q = 1.0`; by alternating small and giant steps with an orderly arrangement of the tiles, the estimate comes down to `≈ 0.94`
(not verified). The theoretical minimum is unknown: a counting argument gives only `q ≳ 0.67`.

*Counting argument (the author's own, in the spirit of Shoup).* In the generic model every element obtained has exponent `a + b·x`
with `x` unknown. The only information about `x` comes from two elements that coincide (or are opposite): a pair
with `b₁ ≠ ±b₂` fixes at most 2 values of `x`, a pair with `|b₁| = |b₂|` at most 1, two elements with `b = 0` (tame) none.
With `n` elements there are at most `n²/2` pairs × 2 values, so the probability of having found `x` after `n` operations is
`≤ n²/W`, and therefore `E[cost] ≥ ∫₀^{√W} (1 − n²/W) dn = (2/3)·√W ≈ 0.67·√W`. It is a bound that errs on the side of optimism (pairs with
`|b₁ − b₂| > 1` almost never give an integer `x` in the interval), not a construction: no known algorithm reaches it.

**Where it is used.** BSGS with `q = 1.0` needs `T = √W/2` entries (≈ 5.7 bytes each): for puzzle 135 that would be `2^66` entries,
out of reach for any machine, and for the open puzzles with a known public key (140–160) `2^69…2^79`. The gain (≈ 6 % of steps compared
with the kangaroo, plus a *deterministic* cost without the long tail of the kangaroo) exists for `W ≲ 2^70` with 128 GB of RAM and is
implemented (section 9). With little memory the best known minimum remains ≈ 1.2–1.3.

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
| `q < 1` with little memory | not known; the best known is ≈ 1.15–1.36 (here 1.06 measured, ≈ 1.04 at the scale of the open puzzles in the model, RESEARCH.md 8.12) |
| `q ≈ 1.0` with a lot of RAM | yes (BSGS with `±`, section 9): `W ≲ 2^70` with 128 GB; better than the kangaroo up to `W ≈ 2^71` |
| exponent `< 1/2` for a single instance, classical | no in the generic model; no specific attack on secp256k1 is known |
| exponent `1/3` per instance | yes with a `W^{2/3}` precomputation and many instances of the same width |
| polynomial time | only with a fault-tolerant quantum computer |

Additional sources: Bernstein–Lange, *Computing small discrete logarithms faster* (<https://eprint.iacr.org/2012/458>);
Corrigan-Gibbs–Kogan, *The Discrete-Logarithm Problem with Preprocessing* (<https://eprint.iacr.org/2017/1113>);
Roetteler et al., *Quantum resource estimates for computing elliptic curve discrete logarithms* (<https://eprint.iacr.org/2017/598>);
Ekerå–Håstad, *Quantum algorithms for computing short discrete logarithms and factoring RSA integers* (<https://eprint.iacr.org/2017/077>).

### 8.4 Cost of a step and hardware bottlenecks

With the algorithm fixed (constant ≈ 1.06 and exponent 1/2, sections 8.1–8.2), what remains is the cost of **one step** and how many
steps per second the hardware can really do.

**The arithmetic minimum.** A step is an affine addition `P + S_j`: slope `λ = (y − y_j)/(x − x_j)`, `x₃ = λ² − x − x_j`,
`y₃ = λ(x − x₃) − y`, i.e. 2M + 1S plus an inversion. With Montgomery's trick on a batch of `n` walkers the inversion costs
3M per element plus `≈ 270/n` M (a single real inversion per batch). Total ≈ **5M + 1S + 270/n M**, plus two normalizations
(choosing the jump and the `±` representative require canonical `x` and `y`). Projective coordinates do not help: to choose
the jump a canonical `x` is needed, which again takes an inversion. The author knows of no way to do fewer field operations per step.

The measured cost of a step on the CPU (AVX-512 IFMA, with two vectors at a time) and on the GPU, the memory traffic and the instruction counts of
the CUDA kernel, the reasons for not keeping the herd in shared memory or in L2, and the cost of a BSGS giant step: RESEARCH.md section 8.4.

**Orders of magnitude for puzzle 140** (8.85·10²⁰ steps on average with schema 3, median ≈ 0.83·10²¹): at 1.03 G steps/s (measured on a
laptop with a discrete GPU, an integrated GPU and the CPU) that is ≈ 27,000 years on average; the probability of succeeding in one year is
≈ `(n/c)² ≈ 10⁻⁹` (model of section 11). Every doubling of speed halves the time, but the order of magnitude remains.

### 8.5 Comparison with the state of the art: puzzle 135 and RCKangaroo

Puzzle 135 (`W = 2^134`) has been solved, reportedly with about 300 GPUs in 5 months (a reported figure, not verified here). Calculations:

* expected steps with `K = 1.15`: `1.15·2^67 ≈ 1.7·10²⁰`;
* 300 RTX 4090 with the fastest public kernel (RCKangaroo: 8 G steps/s declared in the first versions, 14.5 in the latest)
  for 1.3·10⁷ s: `3.1–5.7·10¹⁹` steps, i.e. **18–33 % of the expected effort**;
* in the model of section 11 the probability of finishing within that fraction is ≈ 3–8 %: either it went well, or the resources
  were larger than reported. In any case the numbers add up only with GPUs doing 8–14 G steps/s.

What follows for this program:

1. **Constant**: 1.15 for RCKangaroo, 1.06 here (measured, sections 4.2 and 8.10). The arrangement of the walkers on both sides of zero comes
   from their method (studied and remodeled in the simulator, their code not used); the species with fixed residues and the long jumps with
   banded zones are this program's own (sections 8.8 and 8.10).
2. **Fruitless cycles** (section 4.3): RCKangaroo handles them explicitly (cycles of 2 with a second table of jumps, longer cycles
   reconstructed in a separate kernel). Here they are detected and left by every walker in the same way. Without such handling, with large
   `dp_bits` every walker gets stuck and a campaign on puzzle 140 would never find the key.
3. **Step speed on GPU**: this is the largest gap. RCKangaroo does ≈ 14.5 G steps/s on a 4090 (128 SM at 2.5 GHz). In proportion to integer
   compute capacity, on the reference laptop GPU (20 SM, ≈ 1.4 GHz) it would be ≈ 1–1.3 G steps/s; the kernel here does ≈ 0.81 G steps/s there
   (RESEARCH.md section 8.4). The main differences in their kernel: a state of 24 walkers per thread kept in the L2 cache (on a 4090, 72 MB),
   no 192-bit distance in the loop (only the jump index is written, and the distances are reconstructed in a second kernel), a fast inversion
   (a binary-GCD type algorithm instead of the power `p − 2`) and 64-bit arithmetic in PTX assembly. The kernel here also keeps only a history
   of the jumps and inverts by divstep (section 5), and on cards with a large L2 it tries herds that fit in it; the reference laptop GPU has only
   2 MB of L2, so on that card the state of the walkers has to live in video memory.

### 8.8 Below 1.085: species with fixed residues (schema 2)

In RESEARCH.md 8.7 a pair of walkers with coefficients `b ≠ b'` gave an integer `x` only with probability `1/|b − b'|`, and no congruence
made all the pairs integer together. True pair by pair; but one can **choose for which `x` each pair is integer** and arrange that,
for every `x`, there are enough of them. The layout in use (schema 3, section 8.10) keeps these species and residues.

* Each walker has `E = b·x + u` with `u ≡ r (mod 12)` and **all jumps multiples of 12**: the residue `r` never changes (the change of
  representative `E → −E` maps `(b, r)` to `(−b, −r)`, i.e. to the same species seen from the other side). The start `u` is uniform in the
  zone of the species and rounded down to the residue.
* Two species `(b, r)` and `(b', r')` can fall on the same class only if `r + (b − b')·x ≡ r' (mod 12)` (or the analogous condition
  for `E = −E'`). When the condition holds, the exponents of the two groups lie on the same lattice of step 12 and meet 12 times
  more often; when it does not hold they never meet. The factor of the pair goes from `1/|b − b'|` to `12/|b − b'|` on the good residues of `x`
  and to 0 on the others: with the right species every residue of `x` receives more useful pairs than before.
* `tools/research/kspecies.py` computes the exact coverage (geometry of the zones, the two ways of colliding, the Jensen penalty) and searches for the species:
  first it enumerates the assignments of residue classes to species, then it optimizes the shares. The best one for `b ∈ {0, 1, 3}` (tame walkers,
  wild on `Q`, wild on `3Q`):

| species | residues mod 12 | zone of `u` (in units of `W`) | share |
|---|---|---|---|
| tame, `E = u` | 0, 3, 6, 9 (shares 4.2 %, 9.4 %, 4.2 %, 11.6 %) | `[−0.023, 0.023]` | 29.4 % |
| wild on `Q`, `E = x + u` | 3, 7, 11 | `[−1.005, 0.005]` | 34.9 % |
| wild on `3Q`, `E = 3x + u` | 9 | `[−3.005, 0.005]` | 35.8 % |

  Cost in the random-coverage model: **1.045·√W**, against 1.085 for the optimum without residues (schema 1). Other families
  (`b ∈ {0, 1, 3, 5}` mod 60, `{0, 1, 3, 9}` mod 36) give the same value in the ideal model without geometry (0.991); coefficients 2, 5, 9
  do not help. The value 0.991 is the floor of this idea; the zones overlapping non-uniformly bring it to 1.045.
* **Cost per step: none.** The CPU and GPU step is the same; only the jump table (computed once), the starts
  (`u·G + b·Q'`, with `3·Q'` computed once) and the solution change: a pair with type codes `k₁, k₂` gives `(c₁ − c₂)·x = d₂ − d₁` with
  `c ∈ {0, +1, −1, +3, −3}`, exact division by `|c₁ − c₂| ∈ {1, 2, 3, 4, 6}`. The step writes a 2-bit code (tame or
  wild with its sign) and the host turns it into the 3-bit code of the species, so the GPU kernel is the same for every species. The abscissa
  tag in the table has 61 bits (false positives are verified anyway).
* The walk model, the measurements on the real curve, the compatibility of the log and what remains to be gained: RESEARCH.md section 8.8.

### 8.10 Local walks, long jumps and banded zones (schema 3)

**The defect found.** With `±` classes the walk has no drift: every step moves `|E|` by `±s` (s = jump), so after `n` steps a
walker has explored only a region of width ≈ `√n·s` around its start. The models of RESEARCH.md 8.7 and 8.8 were measured with short walks
(the simulator with `dp = 0` restarted a walker at every cycle of 2 steps, i.e. about every 500 steps), where this does not show. With
long walks two walkers can meet only if their regions overlap, and the number of overlapping tame–wild pairs is about

`P ≈ (s / W^¾) · K^1.5`, with `K` walkers and `n ≈ √W/K` steps each.

With small `P` the coverage depends on a few pairs, which meet many times, wasting the extra meetings. For puzzle 135
(`W = 2^134`) with one laptop GPU and the CPU (`K ≈ 2^19.4`) and the jump of `12·√W` of schemas 1 and 2: **`P ≈ 0.6`**. RCKangaroo does not have the problem
because it runs with tens of millions of walkers on many GPUs (`P` grows as `K^1.5`).

**Measurements.** The simulator and the real curve with the walks of schemas 1 and 2 and of schema 3: RESEARCH.md section 8.10.

**Schema 3.**
* **Mean jump `W^¾/16`** (a multiple of 12 as in schema 2): `P ≈ K^1.5/16`, i.e. 256 with 256 walkers and ≈ 2^25 on puzzle 135, while the
  explored region is `W/(16·√K)`, a fraction `1/(16·√K)` of `W`, much smaller than the start zones. On the test intervals (`W ≤ 2^44`) the jump
  goes from 16·√W (at `2^32`) to 128·√W (at `2^44`), longer than the 12·√W of schemas 1 and 2.
* **Banded zones**: the same species and the same residues as schema 2, with every zone split into bands (tame: 2 bands of `0.02·W`; wild on `Q`: bands of
  `0.1·W`; on `3Q`: bands of `0.2·W`) and a share per band, optimized in the exact coverage model (`tools/research/kbands.py`; in the
  program `src/kang_spec3.inc`, generated from `tools/research/kang_schema3.txt` with `tools/research/kspec_table.py`). The density of the wild walkers on `Q` concentrates
  near the two ends of the zone, that of those on `3Q` in the middle. Coverage model: 1.0455 to 1.0344 (1.0304 with finer bands, not adopted: in the
  walks no difference; a second try on fine grids, RESEARCH.md 8.13, holds at 1.0319 and is not adopted either). Walk model, `W = 2^36`, 4096 walkers, `dp = 0`, 5000 trials, two seeds: **1.015 and 1.021 against 1.044 and
  1.053** for schema 2 (steps; the starting points, free in the model, are 1.6 % of `√W` there: RESEARCH.md 8.12). With 64 walkers the bands do not help (1.063 against 1.054).
* Each walker has its group fixed (coefficient and residue, as in schema 2) and at every start draws the band according to the shares: with
  50 bands and few walkers, a fixed band per walker left some residues uncovered and certain keys without a solution (found
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
  open puzzles the model gives ≈ 1.04 (RESEARCH.md section 8.12, where the cost is split into its parts).

**Compatibility.** The log of schema 3 is `dp.bin` in the `PZDP5` format (section 4.7); a `dp.bin` of an older layout is kept aside by `hunt`
and not used.

## 9. Symmetric BSGS with a RAM table (`--mode bsgs`)

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
* **Table**: **4 bytes per entry**, a 32-bit tag taken from `x.n[1]` (0 is replaced by 1: 0 marks a free cell) and nothing else, in
  **buckets of 16 cells = one 64-byte cache line**, load factor 0.70 (≈ 5.7 bytes per baby step). The bucket of an entry is
  `⌊x.n[0]·B/2^64⌋` (`B` buckets, any number); the entry takes the first free cell of its bucket and, when the bucket is full, of the next
  ones (wrapping around), so the cells of a bucket are always filled in order and a lookup ends at the first bucket whose last cell is
  free. A lookup that finds nothing (all of them but one) reads 1.15 buckets (cache lines) on average, with the 16 tags compared at once
  (SSE2; a plain loop elsewhere). Lines read per lookup that finds nothing (simulated): 1.08 at load 0.65, 1.15 at 0.70, 1.29 at 0.75,
  1.55 at 0.80, 2.14 at 0.85, 3.85 at 0.90; measured time per key covered at equal RAM, relative to 0.75: 1.04 at 0.65, 0.99 at 0.70,
  1.08 at 0.80, 1.38 at 0.85. 0.70 and 0.75 are equivalent at equal RAM, and with a table that RAM does not limit 0.70 is 8 % faster per
  giant step. *Lock-free* insertion with `compare_exchange`, transparent huge pages (`madvise(MADV_HUGEPAGE)`) to reduce TLB misses.
* **The baby index is recomputed, not stored.** A tag that matches gives `b` back by a second, small BSGS over `[1, T]`
  (`bsgs_recover`): a table of `2·⌈√T⌉` entries in the format of 8-byte cells, which keep `b` (built at the first match: 23·√T bytes,
  3 MiB for `T = 2^34`), and `≈ √T/4` giant steps for each sign (`Q_a = b·G` or `Q_a = −b·G`). Measured, one thread: 2 ms for
  `T = 2^34` and 25 ms for `T = 2^40` when `b` is there, 6 and 48 ms when nothing is. Then the keys `c + a·S ± b` are checked against `P`
  as before. A tag matches by chance with probability `2^-32` per occupied cell compared, about 13 per lookup, i.e. `≈ 3·10^-9` per
  giant step: about 0.02 ns per giant step at `T = 2^34` and 0.15 ns at `T = 2^40`, against ≈ 70 ns for the step itself (one thread);
  such a match is counted as a false positive in the statistics. Storing `b` took 40 of the 64 bits of a cell: without it a table holds
  twice as many baby steps in the same RAM, so a search whose table is limited by the RAM needs half the giant steps.
* **Tables of earlier versions** are read and used as they are: `PZBSGS02` (1.11.0 to 1.13.0: 8-byte cells, 24-bit tag and 40-bit `b`,
  8 per bucket, load 0.70, 11.4 bytes per baby step) and `PZBSGS01` (up to 1.10.0: one array of the same cells, linear probing, load
  0.65, 12.3 bytes per baby step). They cannot be converted without computing the baby steps again (a cell keeps 24 bits of `x.n[1]` and
  its home bucket is known only to within its run), and they are written back in their own format.
* **Parallel construction**: blocks of up to 2^22 baby steps taken from an atomic counter; each thread uses the same batch-inversion
  window. Measured on 4 cores (AVX-512): ≈ 40–50 M entries/s (smaller blocks on small tables, so every core works).
* **SIMD lane windows**: 4 (AVX2) or 8 (AVX-512, IFMA) consecutive windows per instruction, with the arithmetic of
  `fev.h`/`fev52.h`, both for the giants and for building the table. At startup each variant must produce exactly the
  x-coordinates of the scalar routine (two calls, so the advance of the centers is covered too), otherwise it is not used;
  `PZ_BSGS_SCALAR=1` disables them. With IFMA the `x` of a window point is made canonical by checking its top limb only (a tight element
  has exact limbs; the full reduction runs for a vector with a lane at or above `2^256 − 2^208`). Here (4 threads, table 2^22): giants from 25.8 to ≈ 50 M/s; the limit is now the random read
  of the table.
* **Search**: the giants are the "keys" 1…Ng of a scheduler identical to the linear one (same permutation from the seed,
  same checkpoints/resume); software `prefetch` at distance 16 hides the RAM latency (a bucket is one line, 1.15 per lookup on average).
* **Persistence (`--table FILE`)**: the table depends only on `G` and `T`, **not on the target or the interval**:
  it is built once, saved (4 KiB header with SHA-256, `T`, x-coordinate of `T·G`, checksum of the body, format `PZBSGS03`; written
  to `.tmp` with the header last, then `rename`) and read back for any later campaign (checks: size, checksum, number of entries,
  256 baby steps recomputed at random). For a fixed `T` the cost of a new key drops to
  `W/(4T)` giants.
* **Automatic selection**: the rule of `--mode auto` is in [`USAGE.md`](USAGE.md) section 4. `T` is fixed at campaign creation and
  saved in `campaign.conf` (`bsgs_entries`).
* CPU and RAM only: the GPUs do not take part (the kangaroo remains the choice for using them).

### 9.3 When it pays off

| `W` | optimal `T` | table RAM | BSGS cost | kangaroo (1.06·√W) |
|-----|-------------|-----------|-----------|--------------------|
| 2^50 | 2^24 | 0.09 GiB | 3.4e7 | 3.6e7 |
| 2^60 | 2^29 | 2.9 GiB | 1.1e9 | 1.1e9 |
| 2^64 | 2^31 | 11 GiB | 4.3e9 | 4.6e9 |
| 2^68 | 2^33 | 46 GiB | 1.7e10 | 1.8e10 |
| 2^70 | 2^34 | 91 GiB | 3.4e10 | 3.6e10 |

With 128 GB of RAM (table ≤ 90 GiB) the optimal BSGS fits up to `W ≈ 2^70.0`; with a table smaller than `√W/2` the cost is
`T + W/(4T)`, which stays below the kangaroo (1.06·√W) for `T ≥ 0.35·√W`, that is up to `W ≈ 2^70.9`.

**Honesty about applicability.** The open puzzles with a known public key (140 … 160) have `W = 2^139…2^159`: `T` would be of the
order of `2^69.5…2^79.5`, unthinkable. For them BSGS cannot help and the kangaroo remains (still ≈ 2^70 steps for 140). BSGS
is useful for intervals with a known public key up to `W ≈ 2^71` and as a "universal solver" with a reusable
precomputed table.

## 10. SIMD lane arithmetic (`avx2v`, `avx512v`, `avx512i`)

The linear search spends about 75% of its time in curve arithmetic (field multiplications) and the rest in hashing.
The scalar 4×64-bit multiplications (MULX/ADX) cost ≈ 37 cycles each on the test machine used below. The `*v` engines instead run the
same arithmetic on **4 (AVX2) or 8 (AVX-512) independent windows** in parallel, one per lane:

* **Representation** (`lib/pzcore/include/pzcore/fev.h`): 9 limbs of 29 bits per element, "structure of arrays" (limb `i` of all lanes is one
  register). `vpmuludq` (32×32→64) does 4 or 8 products per instruction and a column of 9 products of ≤ 2^60 fits in 64 bits.
* With 29-bit limbs a difference `a − b + k·p` that only meets a tight factor in a product (or goes to the hash) skips its carry pass: its
  limbs stay below 2^31 + 2^23, and nine products of such a limb and a tight one still fit in 64 bits. With IFMA a product that waits for
  the previous one is latency-bound, so the forward pass of the batch inversion runs as two interleaved chains (even and odd
  denominators), and a product may take a term to add before its reduction (`pz_fev_mul_add`, `pz_fev_sqr_add`: `a·b + c` with the limbs
  of `c` below 2^55, as `K1 − x` or `K2 − x − y` of tight elements are), which saves the subtraction and the carry pass that would
  follow it; the linear step and the kangaroo step use it for `x3` and `y3`. On AVX-512 (32 vector registers) the products are inlined in
  the loop of the engine. The column sums of a 29-bit product pass through an empty `asm` after every addition: otherwise GCC
  reassociates them, computes the 81 products first and spills them.
* **Lane kangaroo**: `x3` and `y3` of a step are made canonical as in the linear search (the full reduction only for a vector with a lane
  the fast form cannot settle), and the jump table keeps each coordinate `c` as `K1 − c`, so that `x − jx` and `y − jy` are an addition and
  its carries (`K1` is the multiple of `p` that a subtraction adds). The entries are read one row per lane and transposed in registers
  (8×8 words on AVX-512, 4×4 on AVX2).
* **Inversion of a vector**: the lanes are inverted together by Montgomery's trick, one scalar inversion by divsteps and 21 (AVX-512) or
  9 (AVX2) scalar products, instead of an addition chain of 255 vector squarings that wait for each other (`pz_fev_inv`).
* The reduction, the limb bounds, the order of the windows, the lane kangaroo (RESEARCH.md 10.0), the AVX-512 IFMA variant (RESEARCH.md 10.1),
  their verification and the measurements on the test machine mentioned above: RESEARCH.md section 10.

## 11. Time-estimation model (`src/eta.c`)

Estimates are computed from a *realistic rate from the start* (`rate_est` in `eta.c`): no estimate until every
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
* **Kangaroo**: cost `n` with `P(found within n) = 1 − exp(−(n/c)²)`, `c = k/0.8862·√W` with `k = 1.06` (`KANG_C_MEAN`, section 8.10), that is `c ≈ 1.20·√W`. Given that after `n₀` steps it has
  not been found yet: `P(within n₀+e | not found) = 1 − exp(−e(2n₀+e)/c²)` and `e_q = c²·L/(n_q + n₀)` with `L = −ln(1−q)`
  (the form without numerical cancellation).

These probabilities are *conditional* on the key being in the interval and on the program being correct; they say nothing
about whether the funds are still there (check the address with a blockchain explorer).
