# Usage guide

## 1. Commands

| Command | What it does |
|---------|--------------|
| `pzsolve puzzles [--open\|--solved] [--pubkey]` | lists the puzzles 1-160 of the built-in database (status, known public key, address, mean effort); puzzles 161-256, emptied in 2017, are reached with `puzzle N` and `hunt -p N` |
| `pzsolve puzzle N [--rate R]` | data of puzzle N (address, public key, status, range) and **time estimates** (it measures the speed of the CPU of this machine, or you give `--rate` in keys/s, for example `800M` or `1.5G`) |
| `pzsolve hunt …`    | starts or resumes a campaign |
| `pzsolve list [-d DIR]` | all the campaigns of the folder: state, progress, speed, time left |
| `pzsolve info (-c NAME \| -p N)` | prints the report of a campaign (its `State` line is the one `list` shows: `FOUND` if `FOUND.txt` exists, `RUNNING` only while a process holds the campaign lock, so a campaign that was killed is `STOPPED`) |
| `pzsolve where (-c NAME \| -p N) --key HEX` | tells at which point of the visiting order of a linear or BSGS campaign a key comes, and whether it was already scanned |
| `pzsolve split V1 V2 …` | divides the `--part` shares among several computers in proportion to their speeds (for example `split 800M 120M`; see section 8.2) |
| `pzsolve merge (-c NAME \| -p N) -f FILE_OR_FOLDER … [-d DIR] [--mem GiB]` | merges the distinguished points of other computers into a kangaroo campaign (see section 8.1) |
| `pzsolve doctor [-d DIR] [--full]` | checks program, system, CPU, GPU, the saved tuning and profile, campaigns, disk, clock and `PZ_*` variables, and says what is wrong; paste its output when asking for help |
| `pzsolve devices`   | detected CPU, instruction sets, RAM and GPUs (CUDA and OpenCL) |
| `pzsolve selftest`  | built-in correctness checks (primitives, database, SIMD engines, kangaroo, BSGS, GPU) |
| `pzsolve bench [--together [--linear] [-t N] [--gpu L] [--prio-gpu P]] [--gpu-only] [--retune]` | speed of every CPU engine (1…N threads), of the kangaroo, of BSGS and of the GPUs (with bandwidth and whole-integer throughput); `--together`: section 7.9 |
| `pzsolve kstat BITS TRIALS [THREADS [R [DP]]]` | statistics of the kangaroo cost on ranges of 2^BITS keys (R jumps instead of 256 and fixed dp-bits, no restarts: for studying cycles) |
| `pzsolve version`   | version and build commit |

`pzsolve help CMD` (or `pzsolve CMD --help`) shows the options of a command. The program prints its messages in English; long lines are
wrapped at the width of the terminal (128 columns at most).

## 2. Quick start: all the puzzles are already inside

`hunt -p N` creates the campaign `pN` in the folder `./campaigns`; running the same command again **resumes** it (same seed,
no work repeated). For the puzzles with a known public key (140, 145, 150, 155, 160 and all the ones already solved) the public
key is used automatically and the program chooses between BSGS and kangaroo (section 4).

The database holds the address, the public key (if it ever appeared on the blockchain) and, for the solved puzzles, the private
key (which serves as a test bench: `hunt -p 40` finds a known key in a few seconds, and `puzzle N` of a solved puzzle says
`A test run (the key is known): …`). Provenance and checks: [`data/README.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/data/README.md).
**The "solved/open" status is the one at the date the database was created**: before starting a long campaign, check on the
address (blockchain explorer) that the funds are still there. The program warns you if you choose a puzzle that is already solved.

### Puzzles 161-256

The original 2015 transaction had **256** outputs, one per puzzle (puzzle `n` received `n` thousandths of a BTC). In July 2017
(block 475240) the creator spent outputs 161-256 in a single transaction and used the funds to raise the prizes of puzzles
1-160. The addresses 161-256 therefore exist but are **empty**. `puzzles` does not list them; `puzzle 161` shows the address and
`hunt -p 161` searches it all the same (with a warning): useful only as an exercise, since `2^160` keys cannot be exhausted. Puzzle 256
stops at `n − 1` (the group order), because keys beyond it do not exist. Provenance and checks of the addresses: `data/README.md`.

## 3. `hunt`: options

`pzsolve hunt --help` lists every option with its default. What needs more than a line of the help:

* `-p N` with `-r S:E` takes the target of puzzle N and the range of `-r`. A key outside the `-r` range is not reported by the
  linear search, not even when its last block happens to reach it (the engines scan whole lanes).
* `-s` takes 64 hex digits as they are and hashes any other text; without it, or with `auto`, the seed is random.
* `--mem` is in GiB (`--mem 0.25` is 256 MiB). Without it the kangaroo takes 40 % of the free RAM for the table of distinguished points
  (at least 256 MiB, at most 64 GiB) and BSGS chooses a table that fits in 70 % of it.
* A `--chunk-bits` below the default slows down the CPU and above all the GPUs, since every launch receives a single block.

A value that cannot be used stops the command with a message such as `--limit: invalid value '100' (expected an integer between 1 and 99)`,
and an unknown option with `unrecognized option '--foo' (see 'pzsolve help hunt')`.

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

Terms used from here on: **W** is the size of the range (2^(n-1) for puzzle n). A **key** is one candidate of the linear search, a **step**
one kangaroo jump or one BSGS giant step. A **distinguished point** is a point of a kangaroo walk that is stored, about one in 2^dp-bits
(`--dp-bits`). A **work-item** is one GPU thread; with the kangaroo it moves `NK` walkers.

| Situation | Choice |
|-----------|--------|
| address only | `linear`: scan of the whole range (CPU and GPU) |
| known public key, range < 2^20 | `linear` (instantaneous) |
| known public key, a BSGS table that costs no more than the kangaroo, and one of: no usable GPU (or `--no-gpu`), `--bsgs-entries` given, the `--table` file exists, the work lasts only a few seconds | `bsgs`: cost ≈ 1.0·√W, deterministic |
| known public key, otherwise | `kangaroo`: ≈ 1.06·√W, it uses the GPUs as well |

The BSGS table is the one of `--bsgs-entries` or of the existing `--table` file; otherwise the optimal one, or the largest that fits in
70 % of the free RAM (or in `--mem`). A `--table` file that does not exist yet does not count: with `bsgs` it is built and saved.

BSGS uses only CPU and RAM; with a powerful GPU the kangaroo can be faster despite its higher cost: if you know that this is the
case, force `--mode kangaroo`. The mode stays fixed in the campaign (`campaign.conf`). Without a public key only the linear search
applies: `-m kangaroo` or `-m bsgs` stops with `puzzle 71 has no known public key: only -m linear applies`. The kangaroo takes ranges of
2^16 to 2^188 keys (puzzles 190 and above are refused with a message; the exponents of its walkers are 192-bit numbers); a range too
large for BSGS ends with `: use -m kangaroo`.

The BSGS table takes about 11.4 bytes per baby step (cells of 8 bytes, 70 % of them in use); `--bsgs-entries` counts baby steps, not
bytes. Tables of other versions: [`RELEASING.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RELEASING.md#compatibility-of-the-files-between-versions), compatibility table.

## 5. The folder of a campaign

```
campaigns/NAME/
  campaign.conf   immutable parameters (range, target, mode, seed, chunk-bits, baby steps)
  lock            lock (flock) that prevents two processes on the same campaign
  state.dat       checkpoint of the search (set of completed ranges, with checksum; .bak = previous one)
  dp.bin          kangaroo: append-only log of the distinguished points (to resume)
  session.txt     total time and sessions
  report.txt      readable report, rewritten every 5 s (the first time 2 s after the start)
  status.json     state for external programs (includes time left and probabilities), rewritten every 5 s
  stats.csv       time series (one row per minute)
  log.txt         timestamped events (also the error that ended a run)
  FOUND.txt       only when the key is found (permissions 0600)
```

* **Stopping**: Ctrl-C, `kill` or closing the terminal (SIGHUP; under `nohup` it is ignored). The state is saved. A second
  Ctrl-C after half a second exits at once (and loses at most the last minute of work).
* **Resuming**: the same command (`hunt -p N`, or `hunt -c NAME`). The seed and the parameters that define the search
  cannot change: if you give `-s`, `-r`, `-a`, `-k`, `-H`, `-m`, `--chunk-bits` or `--bsgs-entries` different from those of the
  existing campaign, the program **refuses** with an error (it does not silently ignore them); you can however change threads, GPUs,
  ISA, memory and the look of the panel. **`pzsolve list`** shows all the campaigns and their state.
* **Campaign name** (without `-c`): `pN` for a whole puzzle; for a sub-range (`-r`) or a custom target
  `pN-xxxxxxxx` / `custom-xxxxxxxx`, where `xxxxxxxx` depends on puzzle, range and target: **the same command always finds
  the same campaign**. For two independent campaigns on the same range use `-c` with different names (and different seeds). Names
  allow letters, digits, `-`, `_`, `.` (not at the beginning or at the end), at most 80 characters, and not a Windows device name (`con`, `prn`, `aux`,
  `nul`, `com0`-`com9`, `lpt0`-`lpt9`, also with an extension): a campaign folder is copied between computers, so the rule is the same on every system.
  A campaign of a custom target shows its name where a puzzle shows `#N` (`list` shows `-` in the puzzle column).
* **Exit codes of `hunt`**, for scripts: 0 = the key was found (also when the campaign had already found it: see `FOUND.txt`);
  1 = the search finished without finding the key (the whole range, or the whole `--part`, was covered);
  130 = interrupted by Ctrl-C, `kill` or the closing of the terminal (the state is saved, the same command resumes it);
  2 = error: invalid or inconsistent options, campaign in use, unreadable files, a final save of the state that failed, or a stop that
  nothing asked for (every GPU failed and the CPU is off).
  The other commands exit with 0 when they succeed and 2 on an error; `selftest` and `doctor` exit with 1 when a check failed, and
  `merge` with 1 when some file could not be merged (it exits with 0 both when the merge found the key, which it shows and saves in
  `FOUND.txt`, and when there is no collision yet).
* A restart loses at most the work of the blocks in progress (a few seconds per worker); the rest is never redone. In BSGS the
  table is rebuilt (a few minutes) unless you use `--table`.
* A campaign that has already found the key does not start again: it shows `FOUND.txt`.

## 6. The progress panel

On a terminal the program draws a panel of about ten lines **in place**, updated once per second
(log lines scroll above it, the panel stays at the bottom). An example (a linear search on a 4-core test machine, half a minute
after the start; `pzsolve X.Y.Z` is the version):

```
pzsolve X.Y.Z | campaign p71 | puzzle #71 | linear | seed id 330eae54f8407a09 | RUNNING
Target        1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU   range of 2^70 keys
Progress      [..............................] 5.435e-11%   (641.71 M / 1.181e+21 keys)
Speed         23.64 M keys/s   (peak 24.13 M, session average 20.34 M)
              CPU x2: 22.69 M  |  GPU[0]: 943.39 k
Time          this session 31.5s | total 31.5s | session no. 1
Time left     50%: 788,333 years | 90%: 1.4 million years | 100%: 1.6 million years
Chance        1h: 7.2e-09% | 24h: 1.7e-07% | 7d: 1.2e-06% | 30d: 5.2e-06% | 365d: 6.3e-05%
              provisional (settles in ~34 s)
              random order: blocks of 2^36 keys
Ctrl-C: stop and save progress; run the same command again to resume.
```

* **Progress**: fraction covered (linear, BSGS). In the kangaroo the line is `Success prob.`: the **estimated probability of having already
  found the key**, with the steps done and the steps expected on average.
* **Time left**: time still needed, at constant speed, to reach the probability 50 % / 90 % / 100 % (in the kangaroo 99 %
  instead of 100 %, which does not exist). If less than a year is left, the expected date and time are shown too. The speed used is
  meant to be realistic from the very beginning (see below); in BSGS it remains a moving average of ≈ 30 s. Past 5 times the expected effort
  without a collision the kangaroo warns once (`kangaroo: 5x the expected effort without a collision: check the public key (-k) and the range`)
  and the line says `Time left     unknown (beyond the model)`.
* **Provisional estimate**. In the first seconds the displayed speed is misleading: a GPU is counted only when its first launch ends, the
  clocks rise, and a laptop slows down after a few minutes under load. For this reason the time left does not appear until every engine has
  produced something (the line says `Time left     measuring...` and the speed carries the remark `[starting: not all engines are counted yet]`),
  and then starts from the steady-state speed of a previous run of the same configuration, if there is one, otherwise from the one measured
  by the tuning (ALGORITHMS.md section 11). While the estimate settles a line says so: `provisional (from the remembered speed, settles in ~2 min)`,
  `provisional (from the tuning run, settles in ~40 s)`, or `provisional (settles in ~34 s)` when there is nothing to start from. With
  `--no-tune` the remembered speeds are neither used nor updated; `pzsolve doctor` lists them.
* **Chance**: probability of success within the next hour, day, week, month and year at this rate, *assuming that the key is in
  the range* (formulas in [`ALGORITHMS.md`](ALGORITHMS.md) section 11).
* In BSGS, while the table is being built, the panel shows phase 1 of 2 and how many giant steps will follow.
* When the key is found the panel ends with `KEY FOUND`, and a box below it gives the key (`key (hex): 0x…`) and the path of `FOUND.txt`.

Modes: `--ui full` (panel), `--ui line` (a single dynamic line), `--ui log` (one summary line per minute in the log,
suited to files and `nohup`), `--ui auto` (default: panel on a terminal, log otherwise). The single line looks like this (the campaign name
takes the place of `#135` for a custom target; `(provisional)` is the remark above):

```
#135 [....................] 0.0000% 25.55 M steps/s | 50%: 178,231 years 90%: 324,846 years 99%: 459,402 years | P(24h)=1.6e-14% (provisional)
```

and the last line in the log of a finished session says how it ended, for example `final state: #26 FOUND, 18.3350% covered`.
Outside a terminal no control codes are ever emitted. `NO_COLOR=1` removes the bold type. The `status.json` file contains the same numbers for
other programs.

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
start of the kangaroo it says so (`GPU[0]: 131072 kangaroos (1024 work-items x 128), memory used 72.08 MiB of 1.00 GiB`; on an integrated GPU,
`… MiB of system RAM`). The kangaroo uses ≈ 40 MiB for the buffer of distinguished points plus 256 bytes per kangaroo (state and jump history);
the herd is limited to one sixth of the declared memory. NVIDIA cards have less declared memory but more speed: bandwidth, not capacity, is the limit.

### 7.3 Device selection and self-test

`pzsolve devices` lists everything with an index for `--gpu`; an NVIDIA card appears twice (CUDA and OpenCL) and by default is
used **only once, through CUDA**. If the CUDA path does not pass the self-test, the program tries the same card through OpenCL.
`--gpu-api opencl` (or `PZ_GPU_API=opencl`) forces OpenCL, `--gpu-api cuda` CUDA only. Every message about a device names it by its index,
`GPU[n]`.

At startup every device runs a **bit-exact self-test against the CPU** (arithmetic, hash160, point addition, linear search in
all its variants, kangaroo steps with distinguished points and restarts); a device that fails is excluded with a message and the program
continues with the others. The devices are started at the same time, one thread each, and each one writes one line when it is ready, in the
order of the list, for example `GPU[0]: NAME via CUDA, 20 SM, 4.00 GiB, window 256, self-test passed`. If the
tests stop on an error of the driver (not on a result that differs from the CPU's: that excludes the device at once) they are repeated, up to
three attempts with a short pause, as a transient error such as a display reset would otherwise cost the whole campaign a card (the log says
`attempt 1 of 3`). Every candidate key is anyway verified again on the CPU.

### 7.4 First run and kernel tuning

The driver compiles the kernels for your card (with CUDA about 10 s per module; with the tuning of the variants
of the linear kernel about one minute, then the kangaroo variants the first time you use it) and keeps the result in its cache:
from the next runs on, the startup is immediate. With several cards the tuning of the devices runs at the same time, and cards of one type are
measured once (the others read the result: `from earlier tuning`). The tuning is stored in `%LOCALAPPDATA%\pzsolve\gpu_tune.txt` on Windows,
`~/.cache/pzsolve/gpu_tune.txt` on Linux, and is repeated when the program version changes. The default variant stays unless another one is
more than 3 % faster.

The variants (window, walkers per thread, L2-sized herds) are listed in [`ALGORITHMS.md`](ALGORITHMS.md) section 5. `bench` shows,
for every GPU, the variant chosen and its speed. On a **laptop** CPU and GPU share power and cooling: `bench --together` (section 7.9)
finds how many CPU threads and which GPUs pay off.

### 7.5 `bench` and the GPUs

For every GPU `bench` prints, besides the linear search and the kangaroo (with the memory used), the copy bandwidth, the throughput of 32-bit
integer instructions and the L2 cache, and for NVIDIA the two ceilings of the kangaroo they give: how to read them is in [`RESEARCH.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RESEARCH.md) section 8.4.

`bench --gpu-only` skips the CPU tests. `bench --retune` ignores the saved choice of the kernel variants
(`gpu_tune.txt`) and measures it again: useful after a driver update or to see the numbers of all the variants.

### 7.6 Automatic machine profile

The first time you run `hunt` with a new version, on a machine with a usable GPU, the program measures by itself which CPU thread count and
which GPUs work best (the comparison of `bench --together`, section 7.9, about 2 minutes, for the kangaroo or for the linear search according to
the campaign), saves the result and from then on uses it as the default. It does not measure in BSGS, nor with `-t`, `--gpu`, `--no-cpu`,
`--no-gpu` or `--limit`. The profile is in the cache folder, next to the kernel tuning (`%LOCALAPPDATA%\pzsolve\machine_profile.txt`
on Windows, `~/.cache/pzsolve/machine_profile.txt` on Linux) and is tied to the program version, the processor, the number of
cores, the step/engine used and the GPUs with their driver: if one of these changes it is measured again. The log always says what was
applied (`profile of this machine from 2026-10-22: 11 CPU threads, GPU 0 (-t and --gpu override them; --retune measures again)`).
You change your mind like this: `-t`/`--gpu`/`--no-cpu`/`--no-gpu` replace the profile for that run; `--retune` measures it again; `--no-tune`
(or `PZ_NO_PROFILE=1`) neither measures nor uses it. If no configuration beats the default one by more than the noise of the measurement, the
saved profile is "the defaults are already the best" and it is not measured again at every start. Ctrl-C during the measurement ends the program
at once (the search is not started, nothing is saved, and the folder of the measurement is removed). Without a cache folder that can be written
to (no `HOME` or `XDG_CACHE_HOME`, as for a service) the measurement is skipped with one notice, since it could not be kept.

### 7.7 Priority of the threads that serve the cards

Every GPU is served by a thread that launches the kernels, collects the results and launches again. When all the cores are busy computing, that thread may
wait for its turn while the card sits idle; on laptops the effect is mixed up with the shared power limit. The tuning compares normal and
high priority with all the CPU threads and keeps the high one only if it is measurably faster. The outcome goes into the profile (`prio=1`) and the log says it
(`GPU: the threads that serve the GPUs run at high priority (from the machine profile)`). `--prio-gpu yes|no` imposes or excludes it (and it is
not measured); `--prio-gpu auto` is the default. On Windows it is `ABOVE_NORMAL` for the card threads only (which sleep almost all the time
waiting for the kernel: they take no time away from the computation); on Linux it is `nice -5` and needs root or `CAP_SYS_NICE`: without it the
measurement is skipped and `doctor` says so. It is not a promise of speed: if the cause is the power of the laptop and not the wait of the thread,
the measurement will give ~0 % and the priority stays normal.

### 7.8 Using only part of the machine: `--limit X`

Off by default. `hunt -p 71 --limit 50` uses about half of the machine to run cooler and consume less, accepting to go slower. There are no pauses or "work and rest" cycles: the load stays constant, so the fan
does not start and stop in waves. It is achieved with fewer compute units, all at full speed:

* **CPU**: X % of the threads (rounded to the nearest, at least one). With `-t` the X % applies to the threads given. As always (section 8) the threads
  go first on different physical cores: for example on 8 cores and 16 threads, `--limit 50` is 8 threads on 8 cores, `--limit 25` is 4 threads
  on 4 cores, `--limit 75` is 12 threads.
* **GPU**: X % of the work-items of every launch (rounded to blocks of 128, at least one) in both the linear search and the kangaroo: the card works
  all the time, but with fewer blocks in flight. The kangaroo consequently has fewer kangaroos. The log writes it
  (`limit 40%: CPU 6 of 16 threads, GPUs at 40% of the work-items`, then `GPU[0]: limit 40%: 3328 work-items per launch`).
* **BSGS**: only the CPU threads (the table construction also uses fewer threads and takes longer). The GPUs do not take part anyway.
* The machine tuning (about 2 minutes at full load) and the profile are neither used nor updated; the steady-state speed of a limited run
  is remembered separately (`pzsolve doctor` lists it with "limit N%"), so that the time estimates stay realistic. `--retune` with `--limit` is rejected.
* The limit is not saved in the campaign: every run chooses its own, and resuming without `--limit` goes back to the whole machine. The panel
  writes "limit X%" before the speeds, and `report.txt` and `status.json` (`limit_percent`) report it.

The consumption does not scale exactly like X: with fewer active cores the CPU can raise the frequency of the remaining ones, and part of the consumption (RAM, idle card,
fans) stays. If you want a firmer cap, try lower values and watch temperatures and fan (HWiNFO, `nvidia-smi`). On Windows
the "Maximum processor state" in the power options (set to 99 % it usually disables turbo) also holds back the frequency without touching the program.
GPU tests in this project are done with a simulated driver: the effect on the watts of a real card has not been measured.

### 7.9 `bench --together`

It starts, as in a campaign (puzzle 140), the CPU threads and all the GPUs and measures the total speed with
5 numbers of CPU threads (all, three quarters, one per physical core, half of those, none), then switches off in turn each slower GPU
(giving back to the CPU the core that serves it). Every configuration lasts 10 s (2 of settling and 8 of measurement) plus a final repetition
of the first one that estimates the noise. At the end it indicates the best configuration, for example `-t 11 --gpu 0`, if it beats
the default one by more than the noise. `-t N` tries only N CPU
threads, `--gpu LIST` restricts the GPUs, `--linear` measures the linear search instead of the kangaroo. The first `hunt` of a version runs the
same comparison by itself (section 7.6).

### 7.10 Environment variables

The program reads them once, when it starts. Two-state variables (such as `PZ_NO_CUDA`) take effect with any value except empty and `0`; a variable that is
set to the empty string counts as not set. A value that cannot be used (a number that is not a whole decimal number in the range given below, a word that is
not one of those allowed) is reported once, as `NAME=TEXT ignored (...)`, and the program goes on as if the variable were not set.

| Variable | Effect |
|----------|--------|
| `PZ_GPU_API=cuda\|opencl` | same as `--gpu-api` (which takes precedence) |
| `PZ_NO_CUDA=1`, `PZ_NO_OPENCL=1` | do not load that runtime |
| `PZ_GPU_HALF=N` | fix the window of the linear kernel (skips the tuning; the values are in ALGORITHMS.md section 5); a number that the device does not offer is reported for each device |
| `PZ_GPU_NK=N` | fix the variant of the kangaroo kernel (skips the tuning): walkers per thread with the suffixes of ALGORITHMS.md section 5, e.g. `128`, `128b`, `64l` |
| `PZ_GPU_TUNE=0` | no tuning; `=1` runs it also on the devices that are not GPUs, for tests (any other value is refused) |
| `PZ_OPENCL_ALL=1` | use OpenCL CPU devices too (section 7.1) |
| `PZ_NO_PROFILE=1` | same as `--no-tune` |
| `PZ_NO_LARGE_PAGES=1` | do not use large pages (Windows, section 10) |
| `PZ_BENCH_SCALE=0.2` | shortens the measurements of `bench --together` and of the machine profile to a fifth, for tests (a number from 0.01 to 100) |
| `PZ_KANG_MIX=0\|1` | on hyper-threaded cores the first thread can use the vector step of the kangaroo and the other one the scalar BMI2/ADX step, which uses the integer multiplier instead of the vector unit. Without the variable the kangaroo measures both at start-up and uses the mixed one when it is at least 3% faster (a line in the log says so); `=0` never uses it, `=1` always does. `bench` shows the gain and whether it is in use (line "mixed") |
| `XDG_CACHE_HOME` | cache folder (also on Windows); without it `%LOCALAPPDATA%` on Windows, then `HOME` (`~/.cache/pzsolve`) |
| `TMPDIR` | folder of the scratch campaigns of the measurements and of the tests (Linux and macOS; `/tmp` without it) |
| `NO_COLOR` | any value but the empty one: no bold type in the panel |
| `TERM=dumb` | the terminal cannot move the cursor: no panel (`--ui log`) |

The variables for tests and diagnostics (`PZ_KANG_ROUND`, `PZ_KANG_VERBOSE`, `PZ_BSGS_SCALAR`, `PZ_TEST_*`, `PZ_ASSUME_IFMA`, `PZ_EXPECT_SHANI`
and those of the simulated CUDA driver) are described in [`ARCHITECTURE.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/ARCHITECTURE.md) section 5b.

## 8. Performance tips

* Run `pzsolve bench` once: it compares the engines and the number of threads; with a GPU also run `pzsolve bench --together` (section 7.9).
  `--isa auto` picks the fastest engine; `bench` shows them all. Every engine, before being used, passes a quick check with a known key: an
  engine that got it wrong would be excluded.
* The compute threads go first on different physical cores, one per core, and only beyond the number of cores on the second SMT thread of a
  core (on Windows the assignment is a hint to the system, which may move the threads).
* The kangaroo on the CPU uses the same SIMD lanes as the linear search; the variant is chosen by measuring, after a bit-by-bit comparison
  with the reference one. `bench` says which one is in use and measures every variant with 1 thread, one thread per physical core and all the
  logical threads. On hyper-threaded cores the **mixed** mode (the lane step on the first thread of each core, the scalar step on its SMT
  sibling) is chosen by itself when it measures faster; `PZ_KANG_MIX=0` or `=1` turns it off or on.
* The kangaroo wastes less with more RAM (a large `--mem` allows lower `dp-bits`); BSGS is limited only by RAM.
* **Large pages**: for BSGS `echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled` is worth it (often already so).
* **Laptop: the steady-state speed is lower than the peak.** In the first seconds CPU and GPU run at maximum power, then the cooling and the shared
  power limit slow them down (on one laptop the GPU went from ~790 M to ~606 M steps/s in 3 minutes); often the integrated GPU gives less
  than it takes away. The automatic tuning lasts a few seconds per configuration and mostly sees the peak: to choose `-t`, judge the
  "Speed" line after 5-10 minutes of campaign, comparing for example `hunt -p 140` and `hunt -p 140 -t 8`.
* Do not use several processes on the same campaign (the lock prevents it: the second one stops with an error).
* For long sessions: `nohup pzsolve hunt -p N --ui log > /dev/null &` and check `pzsolve list` or `report.txt`.
* **Realism**: `pzsolve puzzle N` estimates the time on the CPU of this machine; the GPUs add up. The puzzles 71+ without a public key
  require 10^20…10^40 keys on average: read those numbers before starting a campaign.

### 8.1 Several computers on one kangaroo campaign

The kangaroo splits among computers better than any other algorithm: the distinguished points of different machines meet like those of the
threads of a single machine, and the speeds add up. Three things must be the same on all the computers and one different:
* **the same**: the puzzle (range and public key), the **seed** (`-s`, 64 hexadecimal digits: it determines the jump table, and two
  walks with different tables never meet) and, for efficiency, `--dp-bits`;
* **different**: `--node N` (0 on the first, 1 on the second, …): it makes the kangaroos start from different points. Without it, two computers with the same seed would redo
  the same steps.

```
computer A:  pzsolve hunt -p 140 -s 5f3a…(64 digits) --dp-bits 43 --node 0
computer B:  pzsolve hunt -p 140 -s 5f3a…(same)      --dp-bits 43 --node 1
```
The seed of A's campaign is in the "Seed" line of `campaigns/p140/report.txt` (and in `campaign.conf`). With `--node` the start-up shows the
**jump table id** (`kangaroo: jump table id 402ef43c (computers working on the same campaign must show the same id)`; `log.txt` always has it):
it must be the same on all the computers. From time to time the `dp.bin` of B is carried to A and, **with A's campaign
stopped**, merged: `pzsolve merge -p 140 -f /path/to/B/p140` (a `dp.bin` file, or the campaign folder, or several `-f`). The command
says how many points are new and whether there is a collision between a "tame" and a "wild" point: in that case it finds the key, writes it to
`FOUND.txt` and shows it. You can also merge the other way round (A into B) or both ways: points already known are recognized. After the merge `hunt`
resumes where it was, with the enriched table.

A file `merge` refuses is named with the reason (section 9.3). For the linear search see section 8.2.

### 8.2 Several computers on a linear search

The linear search visits the blocks of the range in a random order decided by the seed. With `--part Q/T` that order is divided into `T`
shares, in turn (the block in position 0 to share 0, the one in position 1 to share 1, …, then again from the start), and the computer scans
only the shares `Q` that belong to it: the shares of different computers never overlap and together they cover the whole range, without
the computers communicating. The short block at the end of the range, if any, is counted exactly.

```
computer A:  pzsolve hunt -p 71 -s 5f3a…(64 digits) --part 0/2
computer B:  pzsolve hunt -p 71 -s 5f3a…(same)      --part 1/2
```
* **The same on all**: puzzle or range, seed (`-s`: you find it in the "Seed" line of `report.txt`, and with `--part` in the log at startup) and
  `--chunk-bits` (if you do not give it, it depends only on the range, so it is already the same). **Different**: the shares.
* **Computers of different speeds**: give more shares to the faster one. With `pzsolve split 800M 120M` you get the proportional division
  (here `0-86/100` and `87-99/100`); `--part 0-86/100` is the same as `0,1,2,…,86/100`. One share per computer is enough if they are equal.
* **The part belongs to the campaign**: it is decided at creation and does not change (a different `--part` on an existing campaign is refused, use `-c` for
  another one) and resuming without `--part` it stays the same. The state file remembers it too, so a state
  of another share cannot be loaded by mistake. To let another computer continue a part, just copy the campaign folder.
* **What you see**: the progress is that of *your* part (at 100 % you have finished your shares, not the range). The probabilities are
  computed on the whole range and count only the work of this computer; times beyond its part are "n/a" (alone it does not get there).
  `where` tells whether a key falls in your part.
* **If a computer stops**, its shares stay stopped: restart its campaign or copy it elsewhere. **When the part ends** the
  program says so and stops; the key, if it is not there, is in one of the other shares.
* Linear search only. With a known public key (kangaroo) use `--node` and `merge`; BSGS cannot be split.
* At least as many blocks as shares are needed (a smaller `--chunk-bits` if the range is short).

## 9. Troubleshooting

### 9.1 First of all: `pzsolve doctor`

It checks in one page the program, the system, the CPU (quick check of every engine with a known
key), the GPUs (opening the device and a bit-by-bit comparison with the CPU), the saved tuning and profile, the campaigns of the folder
(stopped or running, last save, suspensions suffered), disk space and atomic writing, the clock and the `PZ_*` variables.
Lines marked `[XX]` are errors, `[!!]` warnings. `doctor --full` also runs the whole self-test; `-d DIR` gives the folder of the campaigns.

### 9.2 Sleep and hibernation

If the computer goes to sleep or hibernates while a campaign is running:
* the program notices the jump in time (the panel does not freeze nor show absurd speeds), does not count the suspended time either in the times
  or in the speeds, writes `system suspension detected (about … without computing): not counted in times and speeds; saving the state` in the
  log and saves the state at once;
* if after waking up the driver has reset the GPU and a launch fails, the GPU is reopened, checked again with the same self-test as at startup
  and put back to work (`GPU[0]: device resumed (…)`; the kangaroo herd on the GPU restarts from new positions: only the path since the last
  distinguished point is lost, the points already found are safe). The launches that follow a failure are a quarter of the size they were (a card shared
  with a game or the desktop can be slowed down so much that a launch runs past the 2 s of the watchdog of the system, and would fail again), and grow back
  by themselves; the log says the new size. The wait before a new attempt (up to 30 s) ends at once on Ctrl-C. The GPU is excluded after more than 3
  failures in a row, or more than 40 in all (ten minutes of good launches after a failure reset that count, so that a laptop that sleeps every night
  keeps its card);
* `hunt --keep-awake` prevents Windows (and macOS, through `caffeinate`) from going to sleep while the campaign runs (sleep becomes possible again at the end). On Linux: `systemd-inhibit`.

The state on disk is never at risk: every write is atomic and the checkpoint is every 60 s.

### 9.3 Symptoms

| Symptom | Probable cause |
|---------|----------------|
| `the campaign in … is already running` | another process is using the folder: stop it or use another name (`-c`) |
| `the existing campaign uses mode …` / `--range does not match the range of the existing campaign` / `the given seed does not match the one of the existing campaign` | you changed, when resuming, a parameter that defines the campaign: remove it (the campaign resumes as it was) or use another name with `-c` |
| `final state save FAILED` | disk full or folder not writable: free some space and run again (the previous checkpoint is intact) |
| `main checkpoint unusable` / `no usable checkpoint` | `state.dat` is corrupt: `.bak` is tried; otherwise remove it (the search starts over) |
| `the checkpoint belongs to a different split between computers` / `the checkpoint was written by a campaign with another seed` (followed by `the state … is intact but belongs to another campaign, another seed or another part of the range`) | the state is intact but belongs to another `--part`, another seed or another campaign: do not touch it, use the right campaign or another name with `-c` |
| `file … does not belong to this campaign` | `dp.bin` of another campaign: move it away |
| `file … was written with another jump table (id …, this campaign: …)` | `dp.bin` of the same range written with another seed (or the setup of another computer): move it away to start the distinguished points again |
| `file … was written by a newer version of pzsolve` | the `dp.bin` of the campaign comes from a later version: it is left as it is; use that version |
| `the BSGS table needs … but only … of RAM are available` | little RAM: use `--mem`, a smaller `--bsgs-entries` (at creation) or `--mode kangaroo` |
| `table … is not usable: …` / `table header corrupted` | truncated `--table` file or one from another program: delete it, it will be rebuilt |
| `self-test failed, device excluded` | faulty driver or wrong kernel compiler: update the driver; for an NVIDIA card the program also tries OpenCL; report the case |
| `CUDA: the driver is too old for this PTX` | update the NVIDIA driver (525 or later), or use `--gpu-api opencl` |
| `CUDA driver not found` (in `devices`) | normal without an NVIDIA card; with an NVIDIA card, reinstall the graphics driver |
| `the CPU does not support the instruction set …` | use `--isa auto` or `--isa baseline` |
| `NAME=TEXT ignored (…)` | an environment variable has a value the program cannot use and is ignored: correct or remove it (section 7.10) |
| `distinguished-point table full (RAM limit): halving the density of stored points` | expected in about one run in five when memory sets dp-bits (ALGORITHMS.md section 4.4): the search goes on with a slightly later collision; a larger `--mem` helps only if the RAM is there |
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
  adjustable with `--mem GiB`). Windows must be able to "commit" the memory: with the paging file disabled or small, a very large BSGS
  table may be refused (the program says so); leave the paging file managed by the system.
* **Large pages** (optional, useful only for BSGS with tables ≥ 1 GiB): they need the "Lock pages in memory" right
  (`secpol.msc` → Local Policies → User Rights Assignment → *Lock pages in memory*: add your user, then log out and
  back in) and PowerShell started as administrator. Without them the program uses normal pages; `PZ_NO_LARGE_PAGES=1` disables them.
* **Antivirus**: a program that uses all the CPU for a long time may look suspicious to some antivirus programs. The source code is in the repository and
  `SHA256SUMS` lets you verify that the exe is the published one; if Windows Defender blocks it you can add an exclusion
  for the folder.
* **Building it yourself**: from Linux with MinGW-w64, `make windows` (result in `build/win/pzsolve.exe`); `make wintest` runs the
  tests of the exe under Wine.
