# Usage guide

## 1. Commands

| Command | What it does |
|---------|--------------|
| `pzsolve puzzles [--open\|--solved\|--pubkey]` | lists the 160 puzzles of the built-in database (status, known public key, address, average effort); puzzles 161-256, emptied in 2017, are shown with `puzzle N` |
| `pzsolve puzzle N [--rate R]` | data of puzzle N (address, public key, status, interval) and **time estimates** (it measures the speed of this machine, or you give `--rate` in keys/s) |
| `pzsolve hunt …`    | starts or resumes a campaign |
| `pzsolve list [-d DIR]` | all the campaigns of the folder: status, progress, speed, time left |
| `pzsolve info -c NAME` | prints the report of a campaign |
| `pzsolve where -c NAME --key HEX` | tells at which point of the campaign's path a key will be scanned |
| `pzsolve split V1 V2 …` | divides the `--part` shares among several computers in proportion to their speeds (for example `split 800M 120M`; see section 8.2) |
| `pzsolve merge (-c NAME \| -p N) [-d DIR] -f FILE_OR_FOLDER …` | merges the distinguished points of other computers into a kangaroo campaign (see section 8.1) |
| `pzsolve doctor [-d DIR] [--full]` | checks program, system, CPU, GPU, the saved tuning and profile, campaigns, disk, clock and `PZ_*` variables, and says what is wrong; paste its output when asking for help |
| `pzsolve devices`   | detected CPU, instruction sets, RAM and GPUs (CUDA and OpenCL) |
| `pzsolve selftest`  | built-in correctness checks (primitives, database, SIMD engines, kangaroo, BSGS, GPU) |
| `pzsolve bench [--together [--linear] [-t N] [--gpu L] [--prio-gpu P]] [--gpu-only] [--retune]` | speed of every CPU engine (1…N threads), of the kangaroo, of BSGS and of the GPUs (with bandwidth and whole-integer throughput); `--together` measures CPU+GPU together and says how many CPU threads and which GPUs pay off |
| `pzsolve kstat BITS TRIALS [THREADS [R [DP]]]` | statistics of the kangaroo cost on intervals of 2^BITS keys (R jumps instead of 256 and fixed dp-bits, no restarts: for studying cycles) |
| `pzsolve version`   | version and build commit |

## 2. Quick start: all the puzzles are already inside

```sh
pzsolve puzzles --open            # which puzzles are still unsolved
pzsolve puzzle 71                 # address, interval and how long it would take on this machine
pzsolve hunt -p 71                # go: interval and address come from the database
# … Ctrl-C whenever you like …
pzsolve hunt -p 71                # the same command resumes where you stopped
pzsolve list                      # status of all the campaigns
```

`hunt -p N` creates the campaign `pN` in the folder `./campaigns`; running the same command again **resumes** it (same seed,
no work repeated). For the puzzles with a known public key (140, 145, 150, 155, 160 and all the ones already solved) the public
key is used automatically and the program chooses between BSGS and kangaroo (section 4).

The database holds the address, the public key (if it ever appeared on the blockchain) and, for the solved puzzles, the private
key (which serves as a test bench: `hunt -p 40` finds a known key in a few seconds). Provenance and checks: [`data/README.md`](../../data/README.md).
**The "solved/open" status is the one at the date the database was created**: before starting a long campaign, check on the
address (blockchain explorer) that the funds are still there. The program warns you if you choose a puzzle that is already solved.

### Puzzles 161-256

The original 2015 transaction had **256** outputs, one per puzzle (puzzle `n` received `n` thousandths of a BTC). In July 2017
(block 475240) the creator spent outputs 161-256 in a single transaction and used the funds to raise the prizes of puzzles
1-160. The addresses 161-256 therefore exist but are **empty**. `puzzle 161` shows the address and `hunt -p 161` searches
it all the same (with a warning): useful only as an exercise, since `2^160` keys cannot be exhausted. Puzzle 256 stops at
`n − 1` (the group order), because keys beyond it do not exist. Provenance and checks of the addresses: `data/README.md`.

## 3. `hunt`: options

```
-p, --puzzle N          puzzle 1..256: interval, address and (if known) public key from the built-in database
                        (161..256: the address only, funds withdrawn in 2017, see above). With -r it only
                        selects the target, the interval is the one of -r
-r, --range S:E         custom interval (hexadecimal, inclusive): e.g. a single part of a puzzle
-a, --address ADDR      mainnet address different from the one in the database: P2PKH (`1...`, Base58Check) or native SegWit P2WPKH (`bc1q...`, bech32)
-H, --hash160 HEX       alternative to the address (40 hex digits)
-k, --pubkey HEX        known public key (33 bytes compressed or 65 uncompressed)
-c, --campaign NAME     campaign name: folder <dir>/<NAME>; if it already exists it is resumed [pN with -p N]
-d, --dir DIR           base folder of the campaigns [./campaigns]
-s, --seed SEED         64 hex digits (used verbatim) or any text (hashed); default or `auto`: random
-m, --mode M            auto | linear | kangaroo | bsgs   [auto, see below]
    --table FILE        bsgs: reads the baby-step table from FILE if it exists, otherwise builds it and saves it
    --bsgs-entries N    bsgs: baby steps of the table (when the campaign is created)
    --part Q/T          linear search on several computers: the blocks of the visiting order are divided into T shares and this computer scans share Q
                        (numbered from 0; more than one is allowed: `0,1/4`, `0-6/9`). See "Several computers on a linear search" (section 8.2)
    --node N            kangaroo on several computers: number of this computer (0..65535); see "Several computers on one kangaroo campaign" (section 8.1)
    --prio-gpu P        auto | yes | no: threads that serve the cards run at a higher priority than the compute threads [auto: the tuning tries it]
    --limit X           use only X % of the machine (1..99), always at full speed and without pauses: fewer CPU threads and fewer work-items on the GPUs
                        [off]; see "Using only part of the machine" (section 7.8)
    --retune            measure again the best combination of CPU threads and GPUs of this machine
    --no-tune           do not measure and do not use the machine profile or the remembered speed
    --keep-awake        Windows: prevents the system from going to sleep while the campaign runs
-t, --threads N         CPU threads [available cores; with a GPU one is kept reserved for each]
    --no-cpu / --no-gpu disable the CPU or the GPUs
    --gpu LIST          indices (see `devices`) separated by commas; also allows OpenCL devices on a CPU
    --gpu-api A         auto | cuda | opencl   [auto: CUDA for NVIDIA, OpenCL as a fallback and for the other cards]
    --isa I             auto | baseline | avx2 | shani | avx512 | avx2v | avx512v | avx512i   [auto: micro-benchmark]
                        shani: avx2 with the SHA-256 instructions of the CPU (AMD Zen 2 and later, Intel Ice Lake and later)
                        on arm64: auto | baseline | neon | neon2 (hardware SHA-256 of the CPU, 4 or 8 keys at a time)
    --chunk-bits B      blocks of 2^B keys (20..40) [36, reduced for small intervals]; smaller blocks
                        would slow down the CPU and above all the GPUs (every launch receives a single block)
    --dp-bits B         kangaroo: bits of the distinguished points [automatic]
    --mem GB            kangaroo and bsgs: maximum RAM for the table [kangaroo: 40 % of the free RAM; bsgs: 70 %]
    --ui U              auto | full | line | log   (see section 6)
    --report-interval S seconds between panel updates [1]
    --checkpoint-interval S  seconds between saves [60]
-q / -v                 fewer / more messages
    --new / --resume    fails if the campaign exists / does not exist
```

Examples:

```sh
# CPU + all the GPUs, linear search (known address, no public key)
pzsolve hunt -p 71

# two computers on the same search: same seed, each one its half of the blocks (see "Several computers on a linear search")
pzsolve hunt -p 71 -s 5f3a…(64 digits) --part 0/2
pzsolve hunt -p 71 -s 5f3a…(same)      --part 1/2

# GPUs number 1 and 2 only, seed chosen by you so that the same campaign can be recreated elsewhere
pzsolve hunt -p 71 -c p71b --no-cpu --gpu 1,2 -s "my phrase"

# known public key, BSGS with the table in RAM saved on an SSD and reusable
pzsolve hunt -p 60 --mode bsgs --table /ssd/table-2e29.bin

# custom target
pzsolve hunt -r 40000000000:7fffffffff -a <ADDRESS> -c zone1
```

## 4. Which algorithm? (`--mode auto`)

| Situation | Choice |
|-----------|--------|
| address only | `linear`: scan of the whole interval (CPU and GPU) |
| known public key, interval < 2^20 | `linear` (instantaneous) |
| known public key, the optimal BSGS table (or the largest that fits in 70 % of the free RAM) costs less than the kangaroo, **and there are no GPUs** (or the work lasts only a few seconds, or you already have a `--table`) | `bsgs`: cost ≈ 1.0·√W, deterministic |
| known public key, otherwise | `kangaroo`: ≈ 1.06·√W (campaigns started before 1.7.9: see "Several computers on one kangaroo campaign", section 8.1), it uses the GPUs as well |

BSGS uses only CPU and RAM; with a powerful GPU the kangaroo can be faster despite its higher cost: if you know that this is the
case, force `--mode kangaroo`. The mode stays fixed in the campaign (`campaign.conf`).

## 5. The folder of a campaign

```
campaigns/NAME/
  campaign.conf   immutable parameters (interval, target, mode, seed, chunk-bits, baby steps)
  lock            lock (flock) that prevents two processes on the same campaign
  state.dat       checkpoint of the search (set of completed intervals, with checksum; .bak = previous one)
  dp.bin          kangaroo: append-only log of the distinguished points (to resume)
  session.txt     total time and sessions
  report.txt      readable report, rewritten every 5 s (the first time 2 s after the start)
  status.json     state for external programs (includes time left and probabilities), rewritten every 5 s
  stats.csv       time series (one row per minute)
  log.txt         timestamped events
  FOUND.txt       only when the key is found (permissions 0600)
```

* **Stopping**: Ctrl-C, `kill` or closing the terminal (SIGHUP; under `nohup` it is ignored). The state is saved. A second
  Ctrl-C after half a second exits at once (and loses at most the last minute of work).
* **Resuming**: the same command (`hunt -p N`, or `hunt -c NAME`). The seed and the parameters that define the search
  cannot change: if you give `-s`, `-r`, `-a`, `-k`, `-H`, `-m`, `--chunk-bits` or `--bsgs-entries` different from those of the
  existing campaign, the program **refuses** with an error (it does not silently ignore them); you can however change threads, GPUs,
  ISA, memory and the look of the panel. **`pzsolve list`** shows all the campaigns and their status; `info -p N` / `where -p N --key K` accept
  `-p` in place of `-c pN`.
* **Campaign name** (without `-c`): `pN` for a whole puzzle; for a sub-interval (`-r`) or a custom target
  `pN-xxxxxxxx` / `custom-xxxxxxxx`, where `xxxxxxxx` depends on puzzle, interval and target: **the same command always finds
  the same campaign**. For two independent campaigns on the same interval use `-c` with different names (and different seeds). Names
  allow letters, digits, `-`, `_`, `.` (not at the beginning).
* **Exit codes**: 0 = command succeeded (key found, campaign interrupted with its state saved, or interval exhausted: look at
  `FOUND.txt` or `list`); 1 = the session ended but the final save of the state failed; 2 = error (invalid or
  inconsistent options, campaign in use, unreadable files...) or wrong use of the command line.
* A restart loses at most the work of the blocks in progress (a few seconds per worker); the rest is never redone. In BSGS the
  table is rebuilt (a few minutes) unless you use `--table`.
* A campaign that has already found the key does not start again: it shows `FOUND.txt`.
* **Several computers**: in the linear search use `--part` (section 8.2, "Several computers on a linear search"): no duplicated work and no exchange of
  data; in the kangaroo `--node` and `merge` (section 8.1). With different seeds and without `--part` the campaigns overlap at random.

## 6. The progress panel

On a terminal the program draws a panel of about ten lines **in place**, updated once per second
(log lines scroll above it, the panel stays at the bottom). *Illustrative* example (the numbers are not measurements):

```
pzsolve 1.8.0 | campaign p71 | puzzle #71 | linear | seed 1b0f37cb | RUNNING
Target        1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU   interval of 2^70 keys
Progress      [..............................] 0.0123%   (7.28e19 / 1.181e+21 keys)
Speed         312.40 M keys/s   (peak 330.1 M, session average 308 M)
              CPU x16: 120.1 M  |  GPU0: 192.3 M
Time          this session 12m 3s | total 3d 4h 1m | session no. 4
Time left     50%: 4.1 million years | 90%: 7.4 million years | 100%: 8.2 million years
Chance of finding it from now: 1h 1.2e-12% | 24h 2.9e-11% | 7d 2e-10% | 30d 8.8e-10% | 365d 1.1e-08%
              random order: blocks of 2^36 keys, 12 open intervals
Ctrl-C: stop and save the progress; run the same command again to resume.
```

* **Progress**: fraction covered (linear, BSGS) or the **estimated probability of having already found the key** (kangaroo).
* **Time left**: time still needed, at constant speed, to reach the probability 50 % / 90 % / 100 % (in the kangaroo 99 %
  instead of 100 %, which does not exist). If less than a year is left, the expected date and time are shown too. Since 1.7.4 the speed used is
  meant to be realistic from the very beginning (see below); in BSGS it remains a moving average of ≈ 30 s.
* **Provisional estimate** (since 1.7.4). In the first seconds the displayed speed is misleading: a GPU is counted only when its
  first launch ends, the clocks rise, and a laptop slows down after a few minutes under load (on one laptop: 1.03 G steps/s
  measured cold by the tuning, 834 M after three minutes). For this reason the time left: (1) does not appear until every engine has
  produced something (the line says "measuring" and the speed carries the label "starting"); (2) then uses the average over a
  sliding window of up to 2 minutes, not a moving average started from the first sample; (3) starts from a **remembered speed**, if there is one: the
  *steady-state* speed reached by a previous run of the same configuration (same machine, version, engine, threads, GPUs and priority;
  saved in the profile every 5 minutes and at the end, only after at least 3.5 minutes of running), otherwise the one measured cold by the
  tuning; the measurement replaces the remembered value in about 2 minutes. While the estimate settles, the line "provisional estimate" is shown.
  With `--no-tune` the memory is neither used nor updated. `pzsolve doctor` lists the remembered speeds.
* **Chance of finding it from now**: probability of success in the next hours/days at this rate, *assuming that the key is in
  the interval* (formulas in [`ALGORITHMS.md`](ALGORITHMS.md) section 11).
* In BSGS, while the table is being built, the panel shows phase 1 of 2 and how many giant steps will follow.

Modes: `--ui full` (panel), `--ui line` (a single dynamic line), `--ui log` (one summary line per minute in the log,
suited to files and `nohup`), `--ui auto` (default: panel on a terminal, log otherwise). Outside a terminal no control codes are
ever emitted. `NO_COLOR=1` removes the bold type. The `status.json` file contains the same numbers for other programs.

## 7. GPU: CUDA and OpenCL

### 7.1 Runtimes

The program uses two runtimes, each loaded at startup only if present (with neither it works on the CPU):

* **CUDA** (NVIDIA cards): the CUDA *driver* (`nvcuda.dll` on Windows, `libcuda.so.1` on Linux), installed with the normal
  NVIDIA graphics driver (version **525 or later**). **The CUDA toolkit is not needed**: the kernels are already inside the program as
  PTX (generated by `tools/gen_ptx.py` with NVRTC from the same sources as the OpenCL kernels) and the driver compiles them for your card.
  A Maxwell or newer GPU is required (compute capability ≥ 5.2).
* **OpenCL** (`OpenCL.dll` / `libOpenCL.so.1`): AMD (Adrenalin driver on Windows, ROCm `rocm-opencl-runtime` on Linux), Intel
  (graphics driver / `intel-opencl-icd`); the NVIDIA driver includes OpenCL too. **For tests on CPU only**: `pocl-opencl-icd`; CPU
  devices are not used by default (`PZ_OPENCL_ALL=1 pzsolve selftest` or `--gpu N`).

### 7.2 Declared memory and used memory

`devices` and the startup log show the memory that the driver declares. For a card with its own memory (NVIDIA, discrete AMD) it is the VRAM; for an
**integrated GPU** (Intel UHD/Iris, AMD APUs) the OpenCL driver declares the share of system RAM that the card can address, usually more than half of the RAM
(`59.58 GiB, shared system RAM`): it is neither reserved nor allocated memory. The program uses only as much as its buffers need, and at every
start of the kangaroo it says so (`GPU[2]: 54272 kangaroos (…), memory used 29.8 MiB of system RAM`; on NVIDIA, `… MiB of 4.00 GiB`). The kangaroo uses
≈ 55 MiB for every 1000 work-items of 64 kangaroos (≈ 230 bytes per kangaroo including the jump history), within the limit of one sixth
of the declared memory. NVIDIA cards have less declared memory but more speed: bandwidth, not capacity, is the limit.

### 7.3 Device selection and self-test

`pzsolve devices` lists everything with an index for `--gpu`; an NVIDIA card appears twice (CUDA and OpenCL) and by default is
used **only once, through CUDA**. If the CUDA path does not pass the self-test, the program tries the same card through OpenCL.
`--gpu-api opencl` (or `PZ_GPU_API=opencl`) forces OpenCL, `--gpu-api cuda` CUDA only.

At startup every device runs a **bit-exact self-test against the CPU** (arithmetic, hash160, point addition, linear search in
all its variants, kangaroo steps with distinguished points and restarts); a device that fails is excluded with a message and the program
continues with the others. Every candidate key is anyway verified again on the CPU.

### 7.4 First run and kernel tuning

The driver compiles the kernels for your card (with CUDA about 10 s per module; with the tuning of the six variants
of the linear kernel about one minute, then three kangaroo variants the first time you use it) and keeps the result in its cache:
from the next runs on, the startup is immediate. The tuning is stored in `%LOCALAPPDATA%\pzsolve\gpu_tune.txt` on Windows,
`~/.cache/pzsolve/gpu_tune.txt` on Linux, and is repeated when the program version changes.

The variants tried by the tuning are: linear search with CUDA window 128, 256 or 512 × registers free or capped at 128, with
OpenCL window 64, 128, 256 or 512; kangaroo with 64, 128 or 256 walkers per thread (CUDA) or 32, 64, 128 (OpenCL), each in two
forms (since 1.7.0): plain, and with the products of the batch kept in pairs (`b`, less memory traffic). On cards with an L2 cache of
at least 8 MiB (non-laptop RTX 30/40: 6-72 MiB) the tuning also tries an **L2-sized herd**: fewer work-items (and NK 16 or 32)
so that the private arrays of the walkers fit in half of the L2, where the traffic that limits the kernel does not reach the memory of the
card (`NK 64 L2 herd` in the log; `PZ_GPU_NK=64l`). It is chosen only if measured faster. `bench` shows
the speed of both for every GPU. On a **laptop** CPU and GPU share power and cooling: it is worth comparing for a couple of
minutes the total speed of the panel with the default settings, with `-t 8` (physical cores only: hyper-threading adds
little in the linear search and heats), and with `--no-cpu`, and keeping the fastest. Since 1.7.1 `bench --together` does this (section 7.9).

### 7.5 `bench` and the GPUs

For every GPU `bench` prints, besides the linear search and the kangaroo (with the memory used):
* the **copy bandwidth** (read + coalesced write, in GB/s, with the theoretical bandwidth for NVIDIA): it is the ceiling of the kangaroo, whose
  traffic is ≈ 215 bytes per step;
* the **throughput of 32-bit integer instructions** (`mad.lo`, `mad.hi`, 3-input add) in operations per second and per
  unit and per cycle at the declared clock: it is the other ceiling, because the kangaroo executes ≈ 2200 instructions per step;
* the **L2 cache** and the shared memory per SM, with the ratio between the kangaroo herd and the L2 (if the herd does not fit in the L2, every
  step reads and writes the memory of the card: this is the case of all current GPUs for herds of hundreds of thousands of kangaroos);
* for NVIDIA, the two ceilings computed from those numbers (`bandwidth … steps/s, integer … steps/s`): the measured kangaroo cannot exceed
  the lower one, and if it is close to it the kernel is already at the limit of that resource (a software rework no longer speeds it up).

`bench --gpu-only` skips the CPU tests. `bench --retune` ignores the saved choice of the kernel variants
(`gpu_tune.txt`) and measures it again: useful after a driver update or to see the numbers of all the variants.

### 7.6 Automatic machine profile (since 1.7.1)

The first time you run `hunt` with a new version, on a machine with a GPU and
without `-t`, `--gpu`, `--no-cpu` or `--no-gpu`, the program runs by itself the same comparison as `bench --together` (about 2 minutes, for the
kangaroo or for the linear search according to the campaign), saves the result and from then on uses it as the default: the number of CPU threads
and the GPUs that are worth keeping on. The profile is in the cache folder, next to the kernel tuning (`%LOCALAPPDATA%\pzsolve\machine_profile.txt`
on Windows, `~/.cache/pzsolve/machine_profile.txt` on Linux) and is tied to the program version, the processor, the number of
cores, the step/engine used and the GPUs with their driver: if one of these changes it is measured again. The log always says what was
applied (`machine profile of 2026-10-22: 11 CPU threads, GPU 0`). You change your mind like this: `-t`/`--gpu`/`--no-cpu`/`--no-gpu`
replace the profile for that run; `--retune` measures it again; `--no-tune` (or `PZ_NO_PROFILE=1`) neither measures nor uses it.
If no configuration beats the default one by more than the noise of the measurement, the saved profile is "the defaults are already the
best" and it is not measured again at every start.

### 7.7 Priority of the threads that serve the cards (since 1.7.4)

Every GPU is served by a thread that launches the kernels, collects the results and launches again. When all the cores are busy computing, that thread may
wait for its turn while the card sits idle; on laptops the effect is mixed up with the shared power limit. In the tuning, right after the warm-up and with all the CPU
threads, the program compares normal and high priority in alternating pairs (normal, high, high, normal: a thermal drift cannot
falsify the comparison; the second pair is run only if the first one favors the high priority by at least 1 %) and keeps it only if it wins by
at least 2 % overall and in both pairs. The outcome goes into the profile (`prio=1`) and the log says it (`GPU: threads serving the
cards run at high priority`). `--prio-gpu yes|no` imposes or excludes it (and it is not measured); `--prio-gpu auto` is the default. On Windows
it is `ABOVE_NORMAL` for the card threads only (which sleep almost all the time waiting for the kernel: they take no time away from the computation); on
Linux it is `nice -5` and needs root or `CAP_SYS_NICE`: without it the measurement is skipped and `doctor` says so. It is not a promise of speed:
if the cause is the power of the laptop and not the wait of the thread, the measurement will give ~0 % and the priority stays normal. For tests: `PZ_BENCH_SCALE=0.2` shortens the measurements to a fifth.

### 7.8 Using only part of the machine: `--limit X` (since 1.7.6)

Off by default. `hunt -p 71 --limit 50` uses about half of the machine to run cooler and consume less, accepting to go slower. There are no pauses or "work and rest" cycles: the load stays constant, so the fan
does not start and stop in waves. It is achieved with fewer compute units, all at full speed:

* **CPU**: X % of the threads (rounded to the nearest, at least one). With `-t` the X % applies to the threads given. Threads go first on different physical
  cores, one per core, and only beyond the number of cores on the second SMT thread of the same core: for example on 8 cores and 16 threads, `--limit 50` is
  8 threads on 8 cores, `--limit 25` is 4 threads on 4 cores, `--limit 75` is 12 threads. On Windows the assignment is a hint to the system, which may move the threads.
* **GPU**: X % of the work-items of every launch (rounded to blocks of 128, at least one) in both the linear search and the kangaroo: the card works
  all the time, but with fewer blocks in flight. The kangaroo consequently has fewer kangaroos. The log writes it (`limit 40%: 3328 work-items per launch`).
* **BSGS**: only the CPU threads (the table construction also uses fewer threads and takes longer). The GPUs do not take part anyway.
* The machine tuning (about 2 minutes at full load) and the profile are neither used nor updated; the steady-state speed of a limited run
  is remembered separately (`pzsolve doctor` lists it with "limit N%"), so that the time estimates stay realistic. `--retune` with `--limit` is rejected.
* The limit is not saved in the campaign: every run chooses its own, and resuming without `--limit` goes back to the whole machine. The panel
  writes "limit X%" before the speeds, and `report.txt` and `status.json` (`limit_percent`) report it.

The consumption does not scale exactly like X: with fewer active cores the CPU can raise the frequency of the remaining ones, and part of the consumption (RAM, idle card,
fans) stays. If you want a firmer cap, try lower values and watch temperatures and fan (HWiNFO, `nvidia-smi`). On Windows
the "Maximum processor state" in the power options (set to 99 % it usually disables turbo) also holds back the frequency without touching the program.
GPU tests in this project are done with a simulated driver: the effect on the watts of a real card has not been measured. The value 100 is accepted and means no limit.

### 7.9 `bench --together`

It starts, as in a campaign (puzzle 140), the CPU threads and all the GPUs and measures the total speed with
5 numbers of CPU threads (all, three quarters, one per physical core, half, none), then switches off in turn each slower GPU
(giving back to the CPU the core that serves it). Every configuration lasts 10 s (2 of settling and 8 of measurement) plus a final repetition
of the first one that estimates the noise. At the end it indicates the best configuration, for example `-t 11 --gpu 0`, if it beats
the default one by more than the noise. On a laptop, CPU, integrated GPU and discrete GPU share power and cooling:
often the integrated GPU gives less than it takes away. `-t N` tries only N CPU threads, `--gpu LIST` restricts the GPUs, `--linear` measures the linear search instead of the kangaroo.

### 7.10 Environment variables

Two-state variables (such as `PZ_NO_CUDA`) take effect with any value except empty and `0`.

| Variable | Effect |
|----------|--------|
| `PZ_GPU_API=cuda\|opencl` | same as `--gpu-api` |
| `PZ_NO_CUDA=1`, `PZ_NO_OPENCL=1` | do not load that runtime |
| `PZ_GPU_HALF=128\|256\|512` (CUDA) or `64\|128\|256\|512` (OpenCL) | fix the window of the linear kernel (skips the tuning) |
| `PZ_GPU_NK=64\|128\|256` (CUDA) or `32\|64\|128` (OpenCL) | fix the walkers per thread of the kangaroo kernel (skips the tuning); an optional suffix `b` selects the pairwise products (e.g. `128b`), `l` the L2-sized herd (e.g. `64l`) |
| `PZ_GPU_TUNE=0` | no tuning; `=1` runs it also on the devices that are not GPUs, for tests |
| `PZ_OPENCL_ALL=1` | use OpenCL CPU devices too (section 7.1) |
| `PZ_NO_PROFILE=1` | same as `--no-tune` |
| `PZ_NO_LARGE_PAGES=1` | do not use large pages (Windows, section 10) |
| `PZ_BENCH_SCALE=0.2` | shortens the measurements to a fifth, for tests |
| `XDG_CACHE_HOME` | cache folder (also on Windows) |

For tests and diagnostics: `PZ_KANG_VERBOSE=1` (speed of every step of the chosen kangaroo), `PZ_TEST_DP_CAP=N` (tiny table of distinguished points),
`PZ_MOCK_FAIL_FILE=path` (the simulated CUDA driver fails a launch when the file appears), `PZ_TEST_KTHREAD=N` and
`PZ_TEST_JUMP=N` (for tests only: walkers per CPU thread and mean jump in units of √W).

For the CPU, for diagnostics: `PZ_KANG_ROUND=base|x86|avx2v|avx512v|avx512i|avx512i2` imposes the kangaroo step (`avx512i2`: IFMA on pairs of vectors, since 1.7.7),
`PZ_BSGS_SCALAR=1` disables the lane-based BSGS windows. `PZ_KANG_MIX=1` (experimental): on hyper-threaded cores the first
thread uses the vector step and the other one the scalar BMI2/ADX step, which uses the integer multiplier instead of the vector unit;
`bench` measures whether it pays off (line "mixed") and suggests it.

## 8. Performance tips

* Run `pzsolve bench` once: it compares the engines and the number of threads; with a GPU also run
  `pzsolve bench --together` (about 2 minutes), which measures the real CPU+GPU combination and suggests `-t` and `--gpu`. `--isa auto` chooses by itself among
  `baseline`, `avx2`, `shani` (the same lanes with the SHA-256 stage on the CPU's SHA instructions, only on processors that have them,
  and only if it measures faster), `avx512`, the variants `avx2v`/`avx512v` (curve arithmetic on SIMD lanes) and `avx512i` (the same with
  AVX-512 **IFMA**, on the processors that have it: Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids, Zen 4 and
  later; it is about 1.5 times faster than `avx512v`). Every engine, before being used, passes a quick check
  with a known key: an engine that got it wrong would be excluded.
* The kangaroo on the CPU uses the same SIMD lanes as the linear search (with IFMA ≈ 4 times the scalar step of 1.4.0); the
  variant is chosen by measuring, after a bit-by-bit comparison with the reference one. `bench` says which one is in use and
  measures every variant with 1 thread, one thread per physical core and all the logical threads, plus the "mixed" combination on
  hyper-threading (see `PZ_KANG_MIX`).
* The kangaroo wastes less with more RAM (a large `--mem` allows lower `dp-bits`); BSGS is limited only by RAM.
* **Large pages**: for BSGS `echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled` is worth it (often already so).
* **Laptop: the steady-state speed is lower than the peak.** In the first seconds CPU and GPU run at maximum power, then the cooling and the shared
  power limit slow them down (on one laptop the GPU went from ~790 M to ~606 M steps/s in 3 minutes). The automatic
  tuning lasts a few seconds per configuration and mostly sees the peak: to choose `-t`, judge the "Speed" line after 5-10 minutes of
  campaign, comparing for example `hunt -p 140` and `hunt -p 140 -t 8`.
* Do not use several processes on the same campaign (the lock prevents it: the second one stops with an error).
* For long sessions: `nohup pzsolve hunt -p N --ui log > /dev/null &` and check `pzsolve list` or `report.txt`.
* **Realism**: `pzsolve puzzle N` estimates the time on the CPU of this machine; the GPUs add up. The puzzles 71+ without a public key
  require 10^20…10^40 keys on average: read those numbers before starting a campaign.

### 8.1 Several computers on one kangaroo campaign (since 1.7.3)

The kangaroo splits among computers better than any other algorithm: the distinguished points of different machines meet like those of the
threads of a single machine, and the speeds add up. Three things must be the same on all the computers and one different:
* **the same**: the puzzle (interval and public key), the **seed** (`-s`, 64 hexadecimal digits: it determines the jump table, and two
  walks with different tables never meet) and, for efficiency, `--dp-bits`;
* **different**: `--node N` (0 on the first, 1 on the second, …): it makes the kangaroos start from different points. Without it, two computers with the same seed would redo
  the same steps.

```
computer A:  pzsolve hunt -p 140 -s 5f3a…(64 digits) --dp-bits 43 --node 0
computer B:  pzsolve hunt -p 140 -s 5f3a…(same)      --dp-bits 43 --node 1
```
The seed of A's campaign is in the "Seed" line of `campaigns/p140/report.txt` (and in `campaign.conf`). From time to time the `dp.bin` of B is carried to A and, **with A's campaign
stopped**, merged: `pzsolve merge -p 140 -f /path/to/B/p140` (a `dp.bin` file, or the campaign folder, or several `-f`). The command
says how many points are new and whether there is a collision between a "tame" and a "wild" point: in that case it finds the key, writes it to
`FOUND.txt` and shows it. You can also merge the other way round (A into B) or both ways: points already known are recognized. After the merge `hunt`
resumes where it was, with the enriched table. A log from another campaign (different interval or key) is refused, and so is one written by an older
layout of the kangaroo (versions up to 1.9.0 on campaigns with fewer than 256 kangaroos, that is small intervals): `hunt` keeps such a `dp.bin` as
`dp.bin.vN` and goes on with a new log, `merge` says why it cannot use it. For the linear search see section 8.2.

### 8.2 Several computers on a linear search (since 1.7.5)

The linear search visits the blocks of the interval in a random order decided by the seed. With `--part Q/T` that order is divided into `T`
shares, in turn (the block in position 0 to share 0, the one in position 1 to share 1, …, then again from the start), and the computer scans
only the shares `Q` that belong to it: the shares of different computers never overlap and together they cover the whole interval, without
the computers communicating. The short block at the end of the interval, if any, is counted exactly.

```
computer A:  pzsolve hunt -p 71 -s 5f3a…(64 digits) --part 0/2
computer B:  pzsolve hunt -p 71 -s 5f3a…(same)      --part 1/2
```
* **The same on all**: puzzle or interval, seed (`-s`: you find it in the "Seed" line of `report.txt` and in the log at startup) and `--chunk-bits`
  (if you do not give it, it depends only on the interval, so it is already the same). **Different**: the shares.
* **Computers of different speeds**: give more shares to the faster one. With `pzsolve split 800M 120M` you get the proportional division
  (here `0-86/100` and `87-99/100`); `--part 0-86/100` is the same as `0,1,2,…,86/100`. One share per computer is enough if they are equal.
* **The part belongs to the campaign**: it is decided at creation and does not change (a different `--part` on an existing campaign is refused, use `-c` for
  another one) and resuming without `--part` it stays the same. The state file remembers it too, so a state
  of another share cannot be loaded by mistake. To let another computer continue a part, just copy the campaign folder.
* **What you see**: the progress is that of *your* part (at 100 % you have finished your shares, not the interval). The probabilities are
  computed on the whole interval and count only the work of this computer; times beyond its part are "n/a" (alone it does not get there).
  `where` tells whether a key falls in your part.
* **If a computer stops**, its shares stay stopped: restart its campaign or copy it elsewhere. **When the part ends** the
  program says so and stops; the key, if it is not there, is in one of the other shares.
* Linear search only. With a known public key (kangaroo) use `--node` and `merge`; BSGS cannot be split.
* At least as many blocks as shares are needed (a smaller `--chunk-bits` if the interval is short).

## 9. Troubleshooting

### 9.1 First of all: `pzsolve doctor`

It checks in one page the program, the system, the CPU (quick check of every engine with a known
key), the GPUs (opening the device and a bit-by-bit comparison with the CPU), the saved tuning and profile, the campaigns of the folder
(stopped or running, last save, suspensions suffered), disk space and atomic writing, the clock and the `PZ_*` variables.
Lines marked `[XX]` are errors, `[!!]` warnings. `doctor --full` also runs the whole self-test; `-d DIR` gives the folder of the campaigns.

### 9.2 Sleep and hibernation

If the computer goes to sleep or hibernates while a campaign is running, since 1.7.2:
* the program notices the jump in time (the panel does not freeze nor show absurd speeds), does not count the suspended time either in the times
  or in the speeds, writes `system suspend detected (about …)` in the log and saves the state at once;
* if after waking up the driver has reset the GPU and a launch fails, the GPU is reopened, checked again with the same self-test as at startup
  and put back to work (`device resumed`; the kangaroo herd on the GPU restarts from new positions: only the path since the last
  distinguished point is lost, the points already found are safe). Up to 3 errors in a row and 40 in total, then the GPU is excluded as before;
* `hunt --keep-awake` prevents Windows from going to sleep while the campaign runs (sleep becomes possible again at the end). On Linux: `systemd-inhibit`.

The state on disk is never at risk: every write is atomic and the checkpoint is every 60 s.

### 9.3 Symptoms

| Symptom | Probable cause |
|---------|----------------|
| `campaign in … is already running` | another process is using the folder: stop it or use another name (`-c`) |
| `existing campaign uses mode …` / `--range does not match …` / `the given seed does not match …` | you changed, when resuming, a parameter that defines the campaign: remove it (the campaign resumes as it was) or use another name with `-c` |
| `final state save FAILED` | disk full or folder not writable: free some space and run again (the previous checkpoint is intact) |
| `main checkpoint unusable` / `no usable checkpoint` | `state.dat` is corrupt: `.bak` is tried; otherwise remove it (the search starts over) |
| `the checkpoint belongs to a different split between computers` / `the state … belongs to another campaign or to another part` | the state is intact but belongs to another `--part` or to another campaign: do not touch it, use the right campaign or another name with `-c` |
| `the distinguished-points file … does not belong to this campaign` | `dp.bin` of another campaign: move it away |
| `the BSGS table needs … but only … are available` | little RAM: use `--mem`, a smaller `--bsgs-entries` (at creation) or `--mode kangaroo` |
| `table … unusable` / `corrupt` | truncated `--table` file or one from another version: delete it, it will be rebuilt |
| `self-test failed, device excluded` | faulty driver or wrong kernel compiler: update the driver; for an NVIDIA card the program also tries OpenCL; report the case |
| `CUDA: the driver is too old for this PTX` | update the NVIDIA driver (525 or later), or use `--gpu-api opencl` |
| `CUDA driver not found` (in `devices`) | normal without an NVIDIA card; with an NVIDIA card, reinstall the graphics driver |
| `the CPU does not support the instruction set …` | use `--isa auto` or `--isa baseline` |
| `distinguished-points table full … halving the density` | the RAM limit was reached in a run much longer than expected: the campaign goes on with less efficiency; resume with a larger `--mem` to avoid it |
| very low speed | check `devices` (how many lanes/GPUs are used) and that no other process is running |
| slow first BSGS run | the freshly allocated RAM is "touched" for the first time (on some virtual machines this costs seconds per GiB) |

## 10. Windows

`pzsolve.exe` (from the Windows archive of the release) is a 64-bit static executable for Windows 10/11: it uses only system DLLs and needs no installation. It is used from
**PowerShell** or the Command Prompt, with the same commands and options as on Linux (`.\pzsolve.exe hunt -p 71`, etc.). Double-clicking
shows the help and waits for Enter.

* **Folders**: campaigns go in `campaigns\` in the folder from which you launch the command (or in the one given with `-d`); paths
  with spaces in quotation marks work too (`-d "D:\My campaigns"`). Text files (`report.txt`, `status.json`, ...) open with
  Notepad.
* **Stopping**: Ctrl-C (or Ctrl-Break) saves and exits. So does **closing the window**, logging off or shutting down the PC: Windows grants
  a few seconds and the program uses them to save the state (at most the last minute of work may be lost).
* **Long sessions**: leave the PowerShell window open; the panel updates in place (Windows Terminal or the console of
  Windows 10 1607+). To keep the PC from going to sleep overnight: Settings → System → Power (or use `--keep-awake`, section 9.2).
  On laptops keep the charger connected and the "Best performance" mode.
* **Memory**: the limits are computed on the available RAM reported by Windows (as on Linux: BSGS at most 70 %, kangaroo 40 %,
  adjustable with `--mem GB`). Windows must be able to "commit" the memory: with the paging file disabled or small, a very large BSGS
  table may be refused (the program says so); leave the paging file managed by the system.
* **Large pages** (optional, useful only for BSGS with tables ≥ 1 GiB): they need the "Lock pages in memory" right
  (`secpol.msc` → Local Policies → User Rights Assignment → *Lock pages in memory*: add your user, then log out and
  back in) and PowerShell started as administrator. Without them the program uses normal pages; `PZ_NO_LARGE_PAGES=1` disables them.
* **Antivirus**: a program that uses all the CPU for a long time may look suspicious to some antivirus programs. The source code is in the repository and
  `SHA256SUMS` lets you verify that the exe is the published one; if Windows Defender blocks it you can add an exclusion
  for the folder.
* **Building it yourself**: from Linux with MinGW-w64, `make windows` (result in `build/win/pzsolve.exe`); `make wintest` runs the
  tests of the exe under Wine.
