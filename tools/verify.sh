#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Full verification before a release: unit tests, end-to-end, ASan/UBSan, TSan, Valgrind, aarch64 under QEMU, Windows under Wine.
# Usage: tools/verify.sh [-j N] [quick]      (quick = shorter end-to-end)
#
# The checks are grouped in sections (the table SECTIONS below) that run at the same time, up to N of them (-j N, default the number of
# processors; -j 1 runs them one after the other in the order of the table, which is the order of the checks before they ran in
# parallel). Sections cannot collide: each one has a temporary folder of its own (T), its own cache folder (XDG_CACHE_HOME: the GPU
# tuning and the machine profile) and TMPDIR (the scratch folders of the kangaroo), and a section that builds does so in a build folder
# of its own (build/asan, build/tsan, build/arm64, build/win...); build/ itself is built by the section "build", and the sections that
# run build/pzsolve start when it has ended. The unit tests write their files in the build folder they belong to (PZ_TEST_DIR).
# What a section prints goes to its own log, printed whole when the section ends (never interleaved with another), followed by its
# time. The summary counts the OK and FAIL lines of all the sections; the exit status is the number of problems (0: everything passed).
set -uo pipefail
cd "$(dirname "$0")/.."
export PYTHONDONTWRITEBYTECODE=1   # the Python tools run by several sections at once write nothing next to the sources
QUICK=; JOBS=$(nproc)
while [ $# -gt 0 ]; do
  case $1 in
    quick) QUICK=quick ;;
    -j) JOBS=${2:-}; shift ;;
    -j*) JOBS=${1#-j} ;;
    *) echo "usage: tools/verify.sh [-j N] [quick]"; exit 2 ;;
  esac
  shift
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "verify: -j needs a number of sections (1 or more), not '$JOBS'"; exit 2; }
step() { echo; echo "######## $*"; }
run()  { "$@" || { echo "!!!! FAILED: $*"; fail=$((fail+1)); }; }
ROOT=$(mktemp -d /tmp/pzverify.XXXXXX)
RUNNING=()   # the PIDs of the sections running: stopped with the script
trap 'for p in ${RUNNING[@]+"${RUNNING[@]}"}; do kill "$p" 2>/dev/null; done; rm -rf "$ROOT"' EXIT

vec() { python3 tools/ref.py "$1"; }
field() { vec "$1" | awk -v k="$2" '$1==k{print $2}'; }

K=$(python3 -c "import random; random.seed(5); print(hex(random.randrange(2**25, 2**26)))"); A=$(field "$K" addr)
KK=$(python3 -c "import random; random.seed(15); print(hex(random.randrange(2**43, 2**44)))"); KP=$(field "$KK" pub); KA=$(field "$KK" addr)
MOCK="env LD_LIBRARY_PATH=$PWD/build/mockcuda"   # the simulated CUDA driver (built by the section "build")
# epub PZSOLVE: a public key to encrypt to (for the runs of the sanitizers and Valgrind), made by the binary under test
epub() { "$1" encrypt-keygen --random 2>/dev/null | awk '/^public key/{print $3}'; }
# The AVX-512 IFMA engine is chosen only if CPUID declares IFMA; a processor may execute the instructions while a virtual CPUID hides
# them, and then the tests are told so (PZ_ASSUME_IFMA=1). ifma_env prints the variable to give them (needs build/pzcore/t_fev52).
ifma_env() {
  if build/pzcore/t_fev52 | grep -q OK && ! build/pzcore/t_fev52 | grep -q skipped; then echo PZ_IFMA_UNUSED=1
  elif PZ_ASSUME_IFMA=1 build/pzcore/t_fev52 >/dev/null 2>&1; then echo PZ_ASSUME_IFMA=1
  else echo PZ_IFMA_UNUSED=1; fi
}
# outhas PATTERN COMMAND...: the whole output is captured first (grep -q exiting early would kill the command with SIGPIPE, and pipefail turns that into a failure)
outhas() { local pat=$1 o; shift; o=$("$@" 2>&1); grep -q -- "$pat" <<<"$o"; }
check_found() { grep -q "private key hex: 0x${2#0x}" "$1/FOUND.txt" 2>/dev/null; }
# The timed checks wait for a state with a deadline (WAIT seconds) instead of sleeping a fixed time, so that a loaded machine only makes
# them slower (the helpers are those of tests/e2e.sh)
WAIT=${E2E_WAIT:-60}
wait_for() { local i; for i in $(seq 1 $((WAIT * 10))); do eval "$1" && return 0; sleep 0.1; done; eval "$1"; }   # wait_for 'SHELL TEXT'
wait_gone() { wait_for "! kill -0 $1 2>/dev/null"; }       # wait_gone PID
running() { grep -q '^State: *RUNNING' "$1/report.txt" 2>/dev/null; }   # running CAMPAIGN_DIR: the first report is written 2 s after the start (a stop leaves STOPPED)
run_until() {   # run_until 'SHELL TEXT' COMMAND...: the command runs until the text holds (or it ends by itself), then gets SIGINT; RC is its exit status
  local cond=$1 tp; shift
  timeout -k 60 -s INT 600 "$@" & tp=$!
  wait_for "! kill -0 $tp 2>/dev/null || { $cond; }"
  kill -INT $tp 2>/dev/null; wait $tp; RC=$?
}
# await SECTION: waits until that section has ended (for a section that needs its result only in its last checks; the section awaited
# comes earlier in the table, so it has been started)
await() { while [ ! -e "$ROOT/$1.done" ]; do sleep 1; done; }

# ---- the sections: each one a function sec_NAME, run in a subshell of its own by run_section (below)

# the sources: the puzzle database and the documents (the name of a section is that of its folder, which is in the paths the
# programs print: a check that looks for a word in what they print must not find it in the name of its own section)
sec_sources() {
step "puzzle database (re-checked with the independent implementation)"
run python3 -I -B tools/gen_puzzles.py --check   # -B: nothing written next to the sources (-I ignores PYTHONDONTWRITEBYTECODE)

step "the two user guides in step, and the links of every document (tools/check_docs.sh)"
run tools/check_docs.sh
}

# build/ from scratch (every section that runs build/pzsolve waits for this one), the unit tests, the version, the glibc ceiling
sec_build() {
step "clean build and unit tests"
run make -j"$(nproc)"
[ -x build/pzsolve ] && echo "  OK a plain make builds build/pzsolve" || { echo "  FAIL a plain make does not build build/pzsolve"; fail=$((fail+1)); }
run make test
run make lib-examples >/dev/null
run make lib-cxx >/dev/null
run make mockcuda >/dev/null   # the simulated CUDA driver, for the sections that use it

step "the version a build reports: VERSION for the commit tagged vVERSION and outside git, VERSION-test.COMMIT for any other commit"
VER=$(tr -d '[:space:]' < VERSION); BV=$(build/pzsolve version | awk 'NR==1{print $2}')
if git describe --tags --exact-match --match "v$VER" HEAD >/dev/null 2>&1; then WANTV=$VER; else WANTV=$VER-test.$(git rev-parse --short HEAD); fi
[ "$BV" = "$WANTV" ] && [ "$(make -s print-version)" = "$WANTV" ] && echo "  OK build/pzsolve reports $BV, as make print-version says" || { echo "  FAIL build/pzsolve reports '$BV', expected '$WANTV'"; fail=$((fail+1)); }
# the Makefile alone in a folder of its own: first outside git (a source archive), then as a commit of a new repository, untagged,
# tagged with another version, and tagged vVERSION (lightweight, as the release workflow makes it, and annotated, as tools/release.sh)
VT="$T/version"; mkdir -p "$VT/tools" "$VT/src/kernels/ptx"
cp Makefile VERSION "$VT/"; cp tools/ptx_table.awk "$VT/tools/"; cp src/kernels/ptx/modules.txt "$VT/src/kernels/ptx/"
vgit() { git -C "$VT" -c user.name=verify -c user.email=verify@localhost -c commit.gpgsign=false -c tag.gpgsign=false "$@" >/dev/null 2>&1; }
vmake() { GIT_CEILING_DIRECTORIES="$T" make -s -C "$VT" print-version 2>/dev/null; }
vcase() { local got; got=$(vmake); [ "$got" = "$2" ] && echo "  OK $1: $got" || { echo "  FAIL $1: '$got', expected '$2'"; fail=$((fail+1)); }; }
vcase "outside git" "$VER"
vgit init -q && vgit add -A && vgit commit -q -m one
vcase "an untagged commit" "$VER-test.$(git -C "$VT" rev-parse --short HEAD)"
vgit tag v0.0.1
vcase "a commit tagged with another version" "$VER-test.$(git -C "$VT" rev-parse --short HEAD)"
vgit tag "v$VER"
vcase "the commit tagged v$VER (lightweight tag)" "$VER"
vgit commit -q --allow-empty -m two
vcase "the commit after the tag" "$VER-test.$(git -C "$VT" rev-parse --short HEAD)"
vgit tag -d "v$VER" && vgit tag -a "v$VER" -m "pzsolve $VER"
vcase "the commit tagged v$VER (annotated tag)" "$VER"

step "Linux binary compatibility (no glibc symbol newer than 2.34)"
if r=$(tools/check_binary.sh linux build/pzsolve 2>&1); then echo "  OK $r"; else echo "  FAIL $r"; fail=$((fail+1)); fi
}

# an unoptimized build of the library tests (the field asm needs optimization)
sec_o0() {
step "the library tests, unoptimized (-O0)"
run make OPT=-O0 BUILD=build/o0 lib-test >/dev/null
}

# the committed PTX modules against the kernels (only when PZ_NVRTC is set)
sec_ptx() {
step "PTX modules consistent with the kernels (NVRTC: $PZ_NVRTC)"
run python3 -I tools/gen_ptx.py --nvrtc "$PZ_NVRTC" --check
}

# the AVX-512 IFMA engine, by the CPU, under SDE or emulated
sec_ifma() {
step "AVX-512 IFMA engine (avx512i)"
# The engine is chosen only if CPUID declares IFMA. Here it is tried (1) directly, if the processor executes the
# instructions even when a virtual CPUID hides them (PZ_ASSUME_IFMA=1), and (2) under Intel SDE in Tiger Lake mode
# (variable PZ_SDE = path of sde64), which emulates CPUID and the instructions.
if build/pzcore/t_fev52 | grep -q OK && ! build/pzcore/t_fev52 | grep -q skipped; then echo "  OK t_fev52 (IFMA declared by the CPU)"
elif PZ_ASSUME_IFMA=1 build/pzcore/t_fev52 >/dev/null 2>&1; then
  echo "  OK t_fev52 (IFMA executed by the CPU, hidden by CPUID)"
  PZ_ASSUME_IFMA=1 build/t_engine >/dev/null && echo "  OK t_engine with avx512i" || { echo "  FAIL t_engine with avx512i"; fail=$((fail+1)); }
  rm -rf "$T/i1"; run env PZ_ASSUME_IFMA=1 PZ_ISA=avx512i build/pzsolve hunt -p 26 -a "$A" -c i -d "$T/i1" --ui log -q --no-gpu
  check_found "$T/i1/i" "$K" && echo "  OK linear with PZ_ISA=avx512i" || { echo "  FAIL linear with PZ_ISA=avx512i"; fail=$((fail+1)); }
else echo "  (the CPU does not execute IFMA: tried only under SDE, if available)"; fi
if [ -n "${PZ_SDE:-}" ]; then
  run "$PZ_SDE" -tgl -- build/pzcore/t_fev52
  "$PZ_SDE" -tgl -- build/pzsolve selftest 2>&1 | grep -qE "engine avx512i +OK" && echo "  OK selftest avx512i under SDE (Tiger Lake)" || { echo "  FAIL selftest avx512i under SDE"; fail=$((fail+1)); }
fi

# Without IFMA in the CPU (and without SDE) the two IFMA kangaroo steps and the avx512i engine are tried with an emulation of the two
# instructions (vpmadd52luq/huq with AVX-512F, tools/ifma_emul.h): same arithmetic, slower. It only checks correctness, not speed.
if grep -q avx512f /proc/cpuinfo && ! grep -q avx512ifma /proc/cpuinfo; then
  run make BUILD=build/emu OPT="-O2 -include tools/ifma_emul.h" -j"$(nproc)" build/emu/pzsolve >/dev/null
  EMU=$(PZ_ASSUME_IFMA=1 build/emu/pzsolve selftest 2>&1)
  grep -qE "engine avx512i +OK" <<<"$EMU" && grep -qE "IFMA lanes +identical" <<<"$EMU" && grep -qE "IFMA pairs +identical" <<<"$EMU" && grep -q "SELFTEST PASSED" <<<"$EMU" \
    && echo "  OK emulated IFMA: engine, lane and pair kangaroo steps identical to the reference" || { echo "  FAIL emulated IFMA"; fail=$((fail+1)); }
  for v in avx512i avx512i2; do
    rm -rf "$T/em$v"; run env PZ_ASSUME_IFMA=1 PZ_KANG_ROUND=$v build/emu/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c em -d "$T/em$v" --ui log -q --no-gpu -t 2
    check_found "$T/em$v/em" "$KK" && echo "  OK kangaroo with the $v step (emulated IFMA)" || { echo "  FAIL kangaroo with the $v step (emulated IFMA)"; fail=$((fail+1)); }
  done
fi
}

# the CUDA path with the simulated driver: self-test, campaigns, bench, widths, --prio-gpu, the panel
sec_cuda() {
step "GPU: CUDA path with a simulated driver (same API as the NVIDIA driver, kernels run on the CPU)"
$MOCK build/pzsolve selftest 2>&1 | grep -q "Mock CUDA GPU (CPU) via CUDA  *kernels identical to the CPU" && echo "  OK GPU self-test via CUDA" || { echo "  FAIL GPU self-test via CUDA"; fail=$((fail+1)); }
rm -rf "$T/c1"; run $MOCK PZ_GPU_TUNE=1 build/pzsolve hunt -p 26 -a "$A" -m linear -c c -d "$T/c1" --no-cpu --ui log -q
check_found "$T/c1/c" "$K" && echo "  OK linear via CUDA" || { echo "  FAIL linear via CUDA"; fail=$((fail+1)); }
rm -rf "$T/c2"; run $MOCK build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/c2" --no-cpu --ui log -q
check_found "$T/c2/c" "$KK" && echo "  OK kangaroo via CUDA" || { echo "  FAIL kangaroo via CUDA"; fail=$((fail+1)); }
# the compiled kernel of the kangaroo is built once and kept by the start-up tests (a module load is a JIT compilation by the real driver): the
# simulated driver counts them, two in all (the field test and the kernel), against eight before
rm -rf "$T/c2n"; NLOAD=$($MOCK PZ_MOCK_COUNT=1 build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/c2n" --no-cpu --ui log -q 2>&1 | sed -n 's/.*cuModuleLoadDataEx: \([0-9]*\),.*/\1/p')
[ "${NLOAD:-99}" -le 2 ] && echo "  OK kangaroo via CUDA: $NLOAD modules loaded (the kernel is built once)" || { echo "  FAIL kangaroo via CUDA loads ${NLOAD:-?} modules"; fail=$((fail+1)); }
rm -rf "$T/c3"; outhas "limit 40%: .* work-items per launch" $MOCK PZ_GPU_TUNE=1 build/pzsolve hunt -p 26 -a "$A" -m linear -c c -d "$T/c3" --no-cpu --ui log --limit 40 \
  && check_found "$T/c3/c" "$K" && echo "  OK linear via CUDA with --limit (fewer work-items per launch)" || { echo "  FAIL linear via CUDA with --limit"; fail=$((fail+1)); }
rm -rf "$T/c4"; run $MOCK build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/c4" --no-cpu --ui log -q --limit 40
check_found "$T/c4/c" "$KK" && echo "  OK kangaroo via CUDA with --limit" || { echo "  FAIL kangaroo via CUDA with --limit"; fail=$((fail+1)); }
}

# the bench with the simulated driver, the width of what is printed, --prio-gpu
sec_bench() {
step "bench, widths and --prio-gpu (simulated GPU)"
outhas "copy bandwidth" $MOCK PZ_GPU_TUNE=1 build/pzsolve bench --gpu-only && echo "  OK GPU bench (bandwidth and throughput probes) via CUDA" || { echo "  FAIL GPU bench via CUDA"; fail=$((fail+1)); }
outhas "the first configuration measured again" $MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve bench --together -t 1 && echo "  OK bench --together via CUDA" || { echo "  FAIL bench --together via CUDA"; fail=$((fail+1)); }
outhas "the first configuration measured again" $MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve bench --together --linear -t 1 && echo "  OK bench --together --linear via CUDA" || { echo "  FAIL bench --together --linear via CUDA"; fail=$((fail+1)); }
# nothing printed is longer than 128 columns, GPUs included (log records are one line each when stderr is not a terminal: not counted)
width() { grep -v -E '^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\] ' | awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }'; }
for c in "build/pzsolve devices" "build/pzsolve selftest" "build/pzsolve bench --gpu-only" "build/pzsolve bench --together -t 1" "build/pzsolve bench --together --linear -t 1"; do
  w=$($MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 $c 2>&1 | width)
  [ "$w" -le 128 ] && echo "  OK '${c#build/pzsolve }' via CUDA: lines of at most 128 columns ($w)" || { echo "  FAIL '${c#build/pzsolve }' via CUDA: a line of $w columns"; fail=$((fail+1)); }
done
# priority of the threads serving the GPUs: --prio-gpu is validated and fixes it (no measurement); in auto mode it is measured,
# but only where the system lets the program raise a thread's priority (root / CAP_SYS_NICE on Linux)
build/pzsolve bench --together --prio-gpu maybe >/dev/null 2>&1 && { echo "  FAIL --prio-gpu with an invalid value accepted"; fail=$((fail+1)); } || echo "  OK --prio-gpu: invalid value rejected"
outhas "the first configuration measured again" $MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve bench --together -t 1 --prio-gpu yes && echo "  OK bench --together --prio-gpu yes" || { echo "  FAIL bench --together --prio-gpu yes"; fail=$((fail+1)); }
outhas "in alternating pairs (all CPU threads)" $MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve bench --together -t 1 --prio-gpu no && { echo "  FAIL --prio-gpu no measured the priority"; fail=$((fail+1)); } || echo "  OK --prio-gpu no: priority not measured"
if [ "$(id -u)" = 0 ]; then
  outhas "high priority for the GPU threads: " $MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve bench --together -t 1 && echo "  OK bench --together measures the priority of the GPU threads" || { echo "  FAIL priority measurement"; fail=$((fail+1)); }
  outhas "high priority for the GPU threads: " $MOCK PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve bench --together --linear -t 1 && echo "  OK bench --together --linear measures the priority" || { echo "  FAIL priority measurement (linear)"; fail=$((fail+1)); }
else echo "  (not root: the priority of the GPU threads cannot be raised, measurement not tried)"; fi
}

# the panel of a terminal in the first seconds of a campaign
sec_panel() {
step "the panel's first estimate (simulated GPU, in a pseudo-terminal)"
# the panel's estimate in the first seconds (after the one-time tuning of the simulated GPU, about 50 s here): no completion time until
# every engine (here the simulated GPU too) has produced something,
# then a provisional one that says so (the multi-line panel only appears on a terminal: a pseudo-terminal stands in)
PANEL=$(env LD_LIBRARY_PATH=$PWD/build/mockcuda PZ_GPU_TUNE=1 XDG_CACHE_HOME="$T/panelcache" python3 -I tools/pty_run.py 120 "$PWD/build/pzsolve" hunt -p 71 -m linear -c e -d "$T/panel" -t 2 --ui full 2>&1 || true)
grep -qE "Time left +measuring" <<<"$PANEL" && grep -qE "^ +provisional" <<<"$PANEL" && echo "  OK panel: measuring, then a provisional estimate (CPU + simulated GPU)" || { echo "  FAIL panel: initial estimate"; fail=$((fail+1)); }
}

# the machine profile with the simulated driver
sec_machine() {
step "machine profile (simulated GPU)"
# profile of the machine: measured by the first hunt (CPU + GPU, nothing forced), remembered by the next, applied to -t/--gpu
PC="$T/profcache"; mkdir -p "$PC"; PROF="env XDG_CACHE_HOME=$PC LD_LIBRARY_PATH=$PWD/build/mockcuda PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15"
rm -rf "$T/pr1"; outhas "profile of this machine (just measured)" $PROF build/pzsolve hunt -p 26 -a "$A" -m linear -c p -d "$T/pr1" --ui log && echo "  OK profile measured on the first run (linear)" || { echo "  FAIL profile measured (linear)"; fail=$((fail+1)); }
# a saved verdict that changes nothing goes to the log file only
rm -rf "$T/pr2"; $PROF build/pzsolve hunt -p 26 -a "$A" -m linear -c p -d "$T/pr2" --ui log >/dev/null 2>&1; grep -q "profile of this machine from " "$T/pr2/p/log.txt" && echo "  OK profile reused on the second run (linear)" || { echo "  FAIL profile reused (linear)"; fail=$((fail+1)); }
rm -rf "$T/pr3"; outhas "profile of this machine (just measured)" $PROF build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c p -d "$T/pr3" --ui log && echo "  OK profile measured on the first run (kangaroo)" || { echo "  FAIL profile measured (kangaroo)"; fail=$((fail+1)); }
check_found "$T/pr3/p" "$KK" && echo "  OK kangaroo with CPU+GPU after tuning" || { echo "  FAIL kangaroo with CPU+GPU after tuning"; fail=$((fail+1)); }
# a saved verdict that changes nothing goes to the log file only
rm -rf "$T/pr4"; $PROF build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c p -d "$T/pr4" --ui log >/dev/null 2>&1; grep -q "profile of this machine from " "$T/pr4/p/log.txt" && echo "  OK profile reused on the second run (kangaroo)" || { echo "  FAIL profile reused (kangaroo)"; fail=$((fail+1)); }
rm -rf "$T/pr6"; outhas "high priority" $PROF PZ_PRIO_GPU=yes build/pzsolve hunt -p 26 -a "$A" -m linear -c p -d "$T/pr6" --ui log -t 2 && echo "  OK hunt with PZ_PRIO_GPU=yes (with an explicit -t)" || { echo "  FAIL hunt with PZ_PRIO_GPU=yes"; fail=$((fail+1)); }
check_found "$T/pr6/p" "$K" && echo "  OK linear found with high priority for the GPU threads" || { echo "  FAIL linear with PZ_PRIO_GPU=yes"; fail=$((fail+1)); }
rm -rf "$T/pr5"; outhas "profile\|first run" $PROF build/pzsolve hunt -p 26 -a "$A" -m linear -c p -d "$T/pr5" --ui log -t 2 && { echo "  FAIL explicit -t ignored by the profile"; fail=$((fail+1)); } || echo "  OK explicit -t: no tuning"
# no cache folder to keep the result in (a service without HOME): no measurement of two minutes at every start
rm -rf "$T/pr7"; PO=$(env -u HOME -u XDG_CACHE_HOME LD_LIBRARY_PATH=$PWD/build/mockcuda PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve hunt -p 26 -a "$A" -m linear -c p -d "$T/pr7" --ui log 2>&1)
grep -q "no cache folder that can be written to" <<<"$PO" && ! grep -q "measuring which CPU thread count" <<<"$PO" && echo "  OK no writable cache folder: the machine measurement is skipped" || { echo "  FAIL measurement without a cache folder"; fail=$((fail+1)); }
# Ctrl-C during the first-run measurement (kangaroo: it has a scratch folder): the program stops at once, removes the folder, remembers nothing
# and says how to resume; the campaign itself is not started
rm -rf "$T/pr8" "$T/tmp8" "$T/prof8"; mkdir "$T/tmp8"
env TMPDIR="$T/tmp8" XDG_CACHE_HOME="$T/prof8" LD_LIBRARY_PATH=$PWD/build/mockcuda PZ_GPU_TUNE=1 PZ_BENCH_SCALE=0.15 build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c p -d "$T/pr8" --ui log >"$T/pr8.out" 2>&1 &
PID8=$!
wait_for 'grep -q "warm-up" "$T/pr8.out"'
sleep 0.7; kill -INT $PID8 2>/dev/null   # the signal lands inside the measurement, which the warm-up line says has begun
if ! wait_gone $PID8; then kill -KILL $PID8; echo "  FAIL Ctrl-C during the first-run measurement: still running $WAIT s later"; fail=$((fail+1)); else
  wait $PID8 2>/dev/null
  grep -q "interrupted during the measurement of the machine" "$T/pr8.out" && [ -z "$(ls -A "$T/tmp8")" ] && ! grep -q "kang|" "$T/prof8/pzsolve/machine_profile.txt" 2>/dev/null \
    && echo "  OK Ctrl-C during the first-run measurement: stopped, scratch folder removed, nothing remembered" || { echo "  FAIL Ctrl-C during the first-run measurement"; fail=$((fail+1)); }
fi
}

# doctor, a GPU error mid-campaign and at the start-up tests, two simulated devices
sec_gpudoctor() {
step "doctor, GPU errors and two devices (simulated GPU)"
# doctor, and the protections against suspension: a device error mid-campaign (the mock fails one launch when a file appears,
# like a driver reset after sleep) must reopen the GPU; a frozen process (SIGSTOP, like sleep) must be noticed
outhas "RESULT: " build/pzsolve doctor -d "$T" && echo "  OK doctor" || { echo "  FAIL doctor"; fail=$((fail+1)); }
$MOCK build/pzsolve doctor -d "$T" >/dev/null && echo "  OK doctor with a simulated GPU (no errors)" || { echo "  FAIL doctor with a simulated GPU"; fail=$((fail+1)); }
# doctor --full tests the GPU kernels once (in the full self-test), not in the GPU section and again in the self-test
$MOCK build/pzsolve doctor --full -d "$T" >"$T/dfull.out" 2>&1; DRC=$?
NG=$(grep -c '^  GPU\[0\]' "$T/dfull.out"); NR=$(grep -c 'GPU recovery rules' "$T/dfull.out")
[ $DRC -eq 0 ] && [ "$NG" = 1 ] && [ "$NR" = 1 ] && echo "  OK doctor --full: the GPU tests run once" || { echo "  FAIL doctor --full: GPU[0] test $NG times, recovery rules $NR times (rc=$DRC)"; fail=$((fail+1)); }
gpu_fault_test() {   # MODE PUZZLE: GPU only, the injected error arrives while the campaign runs (a few seconds into the search), the device must be reopened
  local f="$T/fault.$1" d="$T/fl.$1" o="$T/fl.$1.log" tp
  rm -rf "$d" "$f"
  $MOCK PZ_MOCK_FAIL_FILE="$f" PZ_GPU_TUNE=0 timeout -k 60 -s INT 600 build/pzsolve hunt -p "$2" -m "$1" -c f -d "$d" --no-cpu --ui log > "$o" 2>&1 & tp=$!
  wait_for 'running "$d/f"'; sleep 3; touch "$f"
  wait_for 'grep -q "device resumed" "$o"'
  kill -INT $tp 2>/dev/null; wait $tp
  grep -q "device resumed" "$o"
}
gpu_fault_test linear 40 && echo "  OK GPU error mid-campaign: reopened (linear)" || { echo "  FAIL GPU recovery (linear)"; fail=$((fail+1)); }
gpu_fault_test kangaroo 135 && echo "  OK GPU error mid-campaign: reopened (kangaroo)" || { echo "  FAIL GPU recovery (kangaroo)"; fail=$((fail+1)); }
# a transient error of the driver while the start-up tests run (the simulated driver fails the first launch when the file exists at start): the tests
# are repeated and the GPU is used (before, it was left out of the whole campaign); a wrong result still excludes it at once
rm -rf "$T/st1"; touch "$T/st1.fault"
outhas "attempt 1 of 3" $MOCK PZ_MOCK_FAIL_FILE="$T/st1.fault" PZ_GPU_TUNE=1 build/pzsolve hunt -p 26 -a "$A" -m linear -c c -d "$T/st1" --no-cpu --ui log \
  && check_found "$T/st1/c" "$K" && echo "  OK GPU error during the start-up tests: repeated, the GPU is used (linear)" || { echo "  FAIL start-up retry (linear)"; fail=$((fail+1)); }
rm -rf "$T/st2"; touch "$T/st2.fault"
outhas "attempt 1 of 3" $MOCK PZ_MOCK_FAIL_FILE="$T/st2.fault" build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/st2" --no-cpu --ui log \
  && check_found "$T/st2/c" "$KK" && echo "  OK GPU error during the start-up tests: repeated, the GPU is used (kangaroo)" || { echo "  FAIL start-up retry (kangaroo)"; fail=$((fail+1)); }
# two simulated devices (PZ_MOCK_DEVICES=2): their start-up tests run at the same time, one thread each, but the log lines come out in the order of
# the devices and both devices are used; an error of the driver during the tests is repeated for the device it hit; two devices of one model measure
# the kernel variants once (whichever gets there first measures, the other takes the result from the file, as when they started one after the other)
MD2="env LD_LIBRARY_PATH=$PWD/build/mockcuda PZ_MOCK_DEVICES=2"
dev_order() { grep -E 'GPU\[[01]\]: Mock' <<<"$1" | sed -E 's/.*GPU\[([01])\].*/\1/' | tr -d '\n'; }
rm -rf "$T/md1"; MO=$($MD2 PZ_GPU_TUNE=0 build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/md1" --no-cpu --ui log 2>&1)
[ "$(dev_order "$MO")" = 01 ] && grep -q "GPU\[1\]: .* kangaroos" <<<"$MO" && check_found "$T/md1/c" "$KK" \
  && echo "  OK two simulated GPUs, kangaroo: tested together, log lines in the order of the devices, both used" || { echo "  FAIL two simulated GPUs (kangaroo)"; grep -E "GPU\\[|WARN|ERROR" <<<"$MO" | head -n 20; fail=$((fail+1)); }
rm -rf "$T/md2"; MO=$($MD2 PZ_GPU_TUNE=0 build/pzsolve hunt -p 26 -a "$A" -m linear -c c -d "$T/md2" --no-cpu --ui log 2>&1)
[ "$(dev_order "$MO")" = 01 ] && check_found "$T/md2/c" "$K" \
  && echo "  OK two simulated GPUs, linear: log lines in the order of the devices" || { echo "  FAIL two simulated GPUs (linear)"; grep -E "GPU\\[|WARN|ERROR" <<<"$MO" | head -n 20; fail=$((fail+1)); }
rm -rf "$T/md3"; touch "$T/md3.fault"; MO=$($MD2 PZ_MOCK_FAIL_FILE="$T/md3.fault" PZ_GPU_TUNE=0 build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/md3" --no-cpu --ui log 2>&1)
grep -q "attempt 1 of 3" <<<"$MO" && [ "$(dev_order "$MO" | tr -d '\n' | wc -c)" = 2 ] && grep -q "GPU\[1\]: .* kangaroos" <<<"$MO" && check_found "$T/md3/c" "$KK" \
  && echo "  OK two simulated GPUs: an error during the tests is repeated for the device it hit, both are used" || { echo "  FAIL start-up retry with two simulated GPUs"; grep -E "GPU\\[|WARN|ERROR" <<<"$MO" | head -n 20; fail=$((fail+1)); }
rm -rf "$T/md4" "$T/md4c"; MO=$($MD2 PZ_MOCK_SAME_NAME=1 PZ_GPU_TUNE=1 XDG_CACHE_HOME="$T/md4c" build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c c -d "$T/md4" --no-cpu --ui log -v 2>&1)
[ "$(grep -c 'tuning the kangaroo kernel' <<<"$MO")" = 1 ] && [ "$(grep -c 'GPU\[[01]\]: kangaroo with .* (from earlier tuning)' <<<"$MO")" = 1 ] && check_found "$T/md4/c" "$KK" \
  && echo "  OK two simulated GPUs of one model: the kernel variants are measured once" || { echo "  FAIL tuning of two simulated GPUs of one model"; grep -E "GPU\\[|WARN|ERROR" <<<"$MO" | head -n 20; fail=$((fail+1)); }
}

# a frozen process (like a suspension of the system)
sec_suspend() {
step "suspension (a frozen process)"
# the 18 s frozen are the thing measured (the program takes a gap of that size for a suspension); the rest waits for a state, not a time
rm -rf "$T/sus"; ( build/pzsolve hunt -p 45 -m linear -c s -d "$T/sus" --no-gpu --ui log -t 2 > "$T/sus.log" 2>&1 & p=$!; wait_for 'running "$T/sus/s"'; kill -STOP $p; sleep 18; kill -CONT $p; wait_for 'grep -q "system suspension detected" "$T/sus.log"'; kill -INT $p; wait $p ) 2>/dev/null
grep -q "system suspension detected" "$T/sus.log" && echo "  OK frozen process (like a suspension): detected" || { echo "  FAIL suspension detection"; fail=$((fail+1)); }
}

# several computers on one kangaroo campaign: merge, and dp.bin of other versions
sec_merge() {
step "several computers on one kangaroo campaign (merge)"
# several computers on one campaign: same seed and range, different --node; merge adds the other's distinguished points
NS=$(python3 -c "print('ab'*32)"); MA="$T/ma"; MB="$T/mb"
has_dps() { [ "$(stat -c %s "$1/dp.bin" 2>/dev/null || echo 0)" -gt 42 ]; }   # the log has points beyond its 42-byte header
for n in 0 1; do d=$([ $n = 0 ] && echo "$MA" || echo "$MB"); run_until 'has_dps "$d/p140"' build/pzsolve hunt -p 140 -s "$NS" --dp-bits 22 --node $n --no-gpu -t 2 --mem 0.05 -c p140 -d "$d" --ui log -q >/dev/null 2>&1; done
outhas "new, 0 already known" build/pzsolve merge -c p140 -d "$MA" -f "$MB/p140" && echo "  OK merge: points of another node added (no overlap between nodes)" || { echo "  FAIL merge between two nodes"; fail=$((fail+1)); }
outhas "already known" build/pzsolve merge -c p140 -d "$MA" -f "$MB/p140/dp.bin" && echo "  OK repeated merge: all already known" || { echo "  FAIL repeated merge"; fail=$((fail+1)); }
build/pzsolve merge -c p140 -d "$MA" -f "$T/nonexistent.bin" >/dev/null 2>&1 && { echo "  FAIL merge of a nonexistent file gave no error"; fail=$((fail+1)); } || echo "  OK merge: nonexistent file rejected"
# the header of dp.bin carries the jump table: a computer started with another seed walks other paths, merge refuses its log; a log of
# version 4 ("PZDP4", no jump table in the header) is merged as it is
ME="$T/me"; run_until 'has_dps "$ME/p140"' build/pzsolve hunt -p 140 -s "$(python3 -c "print('ef'*32)")" --dp-bits 22 --node 1 --no-gpu -t 2 --mem 0.05 -c p140 -d "$ME" --ui log -q >/dev/null 2>&1
outhas "another jump table" build/pzsolve merge -c p140 -d "$MA" -f "$ME/p140" && echo "  OK merge refuses the log of a computer started with another seed" || { echo "  FAIL merge of a log of another seed"; fail=$((fail+1)); }
{ printf 'PZDP4\n'; tail -c +7 "$MB/p140/dp.bin" | head -c 32; tail -c +43 "$MB/p140/dp.bin"; } > "$T/dp.v4"
outhas "already known" build/pzsolve merge -c p140 -d "$MA" -f "$T/dp.v4" && echo "  OK merge takes a log of version 4" || { echo "  FAIL merge of a log of version 4"; fail=$((fail+1)); }
# a dp.bin written by an older layout of the walkers ("PZDP2", versions up to 1.9.0 on small intervals) cannot be used: hunt keeps it as dp.bin.v2 and goes
# on with a new file, merge refuses it with an explanation
MC="$T/mc"; mkdir -p "$MC/p140"
printf 'PZDP2\n' > "$MC/p140/dp.bin"; head -c 32 /dev/zero >> "$MC/p140/dp.bin"; cp "$MC/p140/dp.bin" "$T/dp.old"
run_until 'running "$MC/p140"' build/pzsolve hunt -p 140 -s "$NS" --dp-bits 22 --node 2 --no-gpu -t 2 --mem 0.05 -c p140 -d "$MC" --ui log >"$T/old.log" 2>&1
grep -q "older layout" "$T/old.log" && cmp -s "$MC/p140/dp.bin.v2" "$T/dp.old" && [ "$(head -c 6 "$MC/p140/dp.bin")" = "$(printf 'PZDP5\n')" ] && echo "  OK hunt keeps a dp.bin of an older kangaroo layout as dp.bin.v2 and goes on with a new log" || { echo "  FAIL old dp.bin not set aside by hunt"; fail=$((fail+1)); }
outhas "older layout" build/pzsolve merge -c p140 -d "$MA" -f "$MC/p140/dp.bin.v2" && echo "  OK merge refuses a log of an older kangaroo layout with an explanation" || { echo "  FAIL merge of an old log"; fail=$((fail+1)); }
# a dp.bin of a NEWER version ("PZDP6") is not an older layout: hunt refuses it and leaves it alone (no dp.bin.v6), merge says where it comes from
MD="$T/md"; mkdir -p "$MD/p140"
printf 'PZDP6\n' > "$MD/p140/dp.bin"; head -c 64 /dev/zero >> "$MD/p140/dp.bin"; cp "$MD/p140/dp.bin" "$T/dp.new"
build/pzsolve hunt -p 140 -s "$NS" --dp-bits 22 --node 3 --no-gpu -t 2 --mem 0.05 -c p140 -d "$MD" --ui log >"$T/new.log" 2>&1 && { echo "  FAIL hunt accepted a dp.bin of a newer version"; fail=$((fail+1)); } \
  || { grep -q "newer version" "$T/new.log" && cmp -s "$MD/p140/dp.bin" "$T/dp.new" && [ ! -e "$MD/p140/dp.bin.v6" ] && echo "  OK hunt refuses a dp.bin of a newer version and leaves it alone" || { echo "  FAIL dp.bin of a newer version"; fail=$((fail+1)); }; }
outhas "newer version" build/pzsolve merge -c p140 -d "$MA" -f "$MD/p140/dp.bin" && echo "  OK merge refuses a log of a newer version with an explanation" || { echo "  FAIL merge of a log of a newer version"; fail=$((fail+1)); }
}

# several computers on one linear search (--part), and --limit
sec_parts() {
step "several computers on one linear search (--part), --limit"
# several computers on one linear search (--part): the shares tile the search order, so exactly one of the two finds the key and the
# other scans its half to the end; the share belongs to the campaign (resuming with another one is refused, resuming without keeps it)
PS=$(python3 -c "print('cd'*32)"); PD="$T/parts"
for part in 0/2 1/2; do build/pzsolve hunt -p 26 -a "$A" -m linear -c "q${part%/*}" -d "$PD" -s "$PS" --chunk-bits 20 --part "$part" --no-gpu -t 2 --ui log -q >/dev/null 2>&1; done
nf=0; for c in q0 q1; do [ -f "$PD/$c/FOUND.txt" ] && nf=$((nf+1)); done
if [ "$nf" = 1 ] && { check_found "$PD/q0" "$K" || check_found "$PD/q1" "$K"; }; then echo "  OK --part 0/2 and 1/2: only one finds the key"; else echo "  FAIL --part: the key was not found by exactly one of the two parts"; fail=$((fail+1)); fi
NF=$([ -f "$PD/q0/FOUND.txt" ] && echo q1 || echo q0)
FF=$([ "$NF" = q0 ] && echo q1 || echo q0)
[ "$(stat -c %a "$PD/$FF/FOUND.txt")" = 600 ] && echo "  OK FOUND.txt created with mode 0600 (it holds the private key)" || { echo "  FAIL FOUND.txt permissions: $(stat -c %a "$PD/$FF/FOUND.txt")"; fail=$((fail+1)); }
grep -q "COMPLETED" "$PD/$NF/report.txt" && grep -q "^Part: " "$PD/$NF/report.txt" && echo "  OK the part without the key is marked completed" || { echo "  FAIL part completed"; fail=$((fail+1)); }
build/pzsolve hunt -p 26 -a "$A" -m linear -c "$NF" -d "$PD" --part $([ "$NF" = q0 ] && echo 1/2 || echo 0/2) --no-gpu -t 2 --ui log -q >/dev/null 2>&1 && { echo "  FAIL a different --part on an existing campaign accepted"; fail=$((fail+1)); } || echo "  OK a different --part on an existing campaign: rejected"
outhas "Part [01]/2 of campaign .* already scanned in full" build/pzsolve hunt -p 26 -a "$A" -m linear -c "$NF" -d "$PD" --no-gpu -t 2 --ui log && echo "  OK resuming without --part keeps the part" || { echo "  FAIL resume without --part"; fail=$((fail+1)); }
build/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c pk -d "$PD" --part 0/2 --no-gpu -t 1 --ui log -q >/dev/null 2>&1 && { echo "  FAIL --part accepted by the kangaroo"; fail=$((fail+1)); } || echo "  OK --part rejected by the kangaroo"
for bad in 2/2 0,0/2 x/2 0/300; do build/pzsolve hunt -p 26 -a "$A" -m linear -c bad -d "$PD" --part "$bad" --no-gpu -t 1 --ui log -q >/dev/null 2>&1 && { echo "  FAIL --part $bad accepted"; fail=$((fail+1)); }; done; echo "  OK invalid --part values rejected"
outhas "not in part" build/pzsolve where -c q0 -d "$PD" --key "$K" || outhas "in part" build/pzsolve where -c q0 -d "$PD" --key "$K" && echo "  OK where reports the part of the key" || { echo "  FAIL where with --part"; fail=$((fail+1)); }

# --limit: a share of the threads (and of the GPU work-items), all flat out: the key is still found, with fewer workers
limit_check() {   # MODE DIR LIMIT THREADS WORKERS EXTRA-ARGS...: finds the key, status.json lists WORKERS CPU workers and the limit, the report says so
  local mode=$1 d=$2 lim=$3 thr=$4 want=$5; shift 5
  rm -rf "$d"; build/pzsolve hunt -p "$LP" -a "$LA" $LK -m "$mode" -c l -d "$d" --limit "$lim" -t "$thr" --no-gpu --ui log -q "$@" >/dev/null 2>&1
  check_found "$d/l" "$LKEY" && [ "$(grep -c '"name": "CPU' "$d/l/status.json")" = "$want" ] && grep -q "\"limit_percent\": $lim," "$d/l/status.json" && grep -q '^Limit: ' "$d/l/report.txt"
}
LP=26 LA=$A LK="" LKEY=$K
limit_check linear "$T/lim1" 50 4 2 && echo "  OK --limit 50 with 4 threads: 2 threads, key found (linear)" || { echo "  FAIL --limit (linear)"; fail=$((fail+1)); }
limit_check linear "$T/lim2" 1 4 1 && echo "  OK --limit 1: at least one thread (linear)" || { echo "  FAIL --limit 1 (linear)"; fail=$((fail+1)); }
LP=44 LA=$KA LK="-k $KP" LKEY=$KK
limit_check kangaroo "$T/lim3" 50 2 1 && echo "  OK --limit 50 with 2 threads: 1 thread, key found (kangaroo)" || { echo "  FAIL --limit (kangaroo)"; fail=$((fail+1)); }
limit_check bsgs "$T/lim4" 50 4 2 && echo "  OK --limit 50 with 4 threads: 2 threads, key found (bsgs)" || { echo "  FAIL --limit (bsgs)"; fail=$((fail+1)); }
rm -rf "$T/lim5"; build/pzsolve hunt -p 26 -a "$A" -m linear -c l -d "$T/lim5" --limit 100 -t 4 --no-gpu --ui log -q >/dev/null 2>&1; LRC=$?
[ $LRC -eq 2 ] && [ ! -e "$T/lim5" ] && echo "  OK --limit 100 refused (1..99; no --limit is no limit)" || { echo "  FAIL --limit 100 (rc=$LRC)"; fail=$((fail+1)); }
for bad in 0 101 x 5.5; do build/pzsolve hunt -p 26 -a "$A" -m linear -c bad -d "$T/limbad" --limit "$bad" --no-gpu --ui log -q >/dev/null 2>&1 && { echo "  FAIL --limit $bad accepted"; fail=$((fail+1)); }; done; echo "  OK invalid --limit values rejected"
build/pzsolve hunt -p 26 -a "$A" -m linear -c bad -d "$T/limbad" --limit 50 --retune --no-gpu --ui log -q >/dev/null 2>&1 && { echo "  FAIL --limit with --retune accepted"; fail=$((fail+1)); } || echo "  OK --limit with --retune: rejected"
}

# tests/e2e.sh
sec_e2e() {
step "end-to-end"
if [ "$QUICK" = quick ]; then E2E_QUICK=1 run tests/e2e.sh build/pzsolve; else run tests/e2e.sh build/pzsolve; fi
}

# ASan/UBSan: build, self-test, unit tests, library, campaigns
sec_asan() {
step "AddressSanitizer + UndefinedBehaviorSanitizer"
run make asan -j"$(nproc)" >/dev/null
export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
run build/asan/pzsolve selftest
await build; IFMA_ENV=$(ifma_env); EPUB=$(epub build/asan/pzsolve)
ASAN_TESTS="t_core t_env t_encrypt t_jobq t_engine t_puzzles t_eta t_bsgs t_fuzz t_ui t_lz t_kernels"
run make BUILD=build/asan OPT="-O1 -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer" $(for t in $ASAN_TESTS; do echo build/asan/$t; done) >/dev/null
for t in $ASAN_TESTS; do run env "$IFMA_ENV" build/asan/$t >/dev/null && echo "  OK $t (ASan/UBSan)"; done
# the library on its own (its tests and examples link nothing but libpzcore.a)
run env "$IFMA_ENV" make BUILD=build/asan OPT="-O1 -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer" lib-test lib-examples >"$T/asan-lib.log" 2>&1 \
  && echo "  OK pzcore tests and examples (ASan/UBSan)" || tail -20 "$T/asan-lib.log"
rm -rf "$T/a1"; run build/asan/pzsolve hunt -p 26 -a "$A" -c a -d "$T/a1" --ui log -q --no-gpu
check_found "$T/a1/a" "$K" && echo "  OK linear (ASan)" || { echo "  FAIL linear (ASan)"; fail=$((fail+1)); }
rm -rf "$T/a9"; run build/asan/pzsolve hunt -p 26 -a "$A" -c a -d "$T/a9" --ui log -q --no-gpu --encrypt-pubkey "$EPUB" --on-found "true"
grep -q '^PZENC1:' "$T/a9/a/FOUND.txt" && ! grep -q -i "${K#0x}" "$T/a9/a/FOUND.txt" && echo "  OK encrypted key (ASan)" || { echo "  FAIL encrypted key (ASan)"; fail=$((fail+1)); }
rm -rf "$T/a2"; run build/asan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c a -d "$T/a2" --ui log -q --no-gpu
check_found "$T/a2/a" "$KK" && echo "  OK kangaroo (ASan)" || { echo "  FAIL kangaroo (ASan)"; fail=$((fail+1)); }
rm -rf "$T/a5"; run build/asan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m bsgs -c a -d "$T/a5" --ui log -q --no-gpu
check_found "$T/a5/a" "$KK" && echo "  OK bsgs (ASan)" || { echo "  FAIL bsgs (ASan)"; fail=$((fail+1)); }
rm -rf "$T/a6"; run build/asan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m bsgs --table "$T/a6.tbl" -c a -d "$T/a6" --ui log -q --no-gpu
rm -rf "$T/a7"; run build/asan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m bsgs --table "$T/a6.tbl" -c a -d "$T/a7" --ui log -q --no-gpu
check_found "$T/a7/a" "$KK" && echo "  OK bsgs with table from file (ASan)" || { echo "  FAIL bsgs with table from file (ASan)"; fail=$((fail+1)); }
rm -rf "$T/a3"; PZ_TEST_DP_CAP=64 run build/asan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c a -d "$T/a3" --ui log -q --no-gpu --mem 0.002 --dp-bits 3
check_found "$T/a3/a" "$KK" && echo "  OK kangaroo with a tiny table (growth + density reduction)" || { echo "  FAIL kangaroo tiny table"; fail=$((fail+1)); }
# the table of a huge interval with a tiny --dp-bits: the number of points expected does not fit in 64 bits (float-cast-overflow)
rm -rf "$T/a8"; run_until 'grep -q "table 32.00 MiB" "$T/a8/a/log.txt" 2>/dev/null' build/asan/pzsolve hunt -p 140 --dp-bits 2 --mem 0.05 -c a -d "$T/a8" --ui log -q --no-gpu -t 1 >"$T/a8.out" 2>&1
! grep -q "runtime error" "$T/a8.out" && grep -q "table 32.00 MiB (max 51.20 MiB)" "$T/a8/a/log.txt" && echo "  OK kangaroo, puzzle 140 with --dp-bits 2 (ASan/UBSan)" || { echo "  FAIL kangaroo, puzzle 140 with --dp-bits 2 (ASan/UBSan)"; fail=$((fail+1)); }
}

# ASan/UBSan: the OpenCL path (pocl when it is installed) and the CUDA path (simulated driver)
sec_asanocl() {
step "AddressSanitizer + UndefinedBehaviorSanitizer: GPU paths"
export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
rm -rf "$T/a4"; run env PZ_OPENCL_ALL=1 build/asan/pzsolve hunt -p 26 -a "$A" -c a -d "$T/a4" --ui log -q --gpu 0 --no-cpu
check_found "$T/a4/a" "$K" && echo "  OK linear on OpenCL (ASan)" || echo "  (OpenCL not available or not used: ignored)"

$MOCK ASAN_OPTIONS=detect_leaks=1 build/asan/pzsolve selftest 2>&1 | grep -q "Mock CUDA GPU (CPU) via CUDA  *kernels identical to the CPU" && echo "  OK GPU self-test via CUDA (ASan)" || { echo "  FAIL GPU self-test via CUDA (ASan)"; fail=$((fail+1)); }
}

# TSan: build, campaigns, interruption, library
sec_tsan() {
step "ThreadSanitizer"
run make tsan -j"$(nproc)" >/dev/null
export TSAN_OPTIONS=halt_on_error=1
await build; IFMA_ENV=$(ifma_env); EPUB=$(epub build/tsan/pzsolve)
rm -rf "$T/t1"; run build/tsan/pzsolve hunt -p 26 -a "$A" -c t -d "$T/t1" --ui log -q --no-gpu -t 4
check_found "$T/t1/t" "$K" && echo "  OK linear (TSan)" || { echo "  FAIL linear (TSan)"; fail=$((fail+1)); }
rm -rf "$T/t5"; run build/tsan/pzsolve hunt -p 26 -a "$A" -c t -d "$T/t5" --ui log -q --no-gpu -t 4 --encrypt-pubkey "$EPUB" --on-found "true"
grep -q '^PZENC1:' "$T/t5/t/FOUND.txt" && echo "  OK encrypted key and --on-found (TSan)" || { echo "  FAIL encrypted key (TSan)"; fail=$((fail+1)); }
rm -rf "$T/t2"; run build/tsan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c t -d "$T/t2" --ui log -q --no-gpu
check_found "$T/t2/t" "$KK" && echo "  OK kangaroo (TSan)" || { echo "  FAIL kangaroo (TSan)"; fail=$((fail+1)); }
rm -rf "$T/t4"; run build/tsan/pzsolve hunt -p 44 -a "$KA" -k "$KP" -m bsgs -c t -d "$T/t4" --ui log -q --no-gpu -t 4
check_found "$T/t4/t" "$KK" && echo "  OK bsgs (TSan)" || { echo "  FAIL bsgs (TSan)"; fail=$((fail+1)); }
rm -rf "$T/t3"; (timeout -s INT 6 build/tsan/pzsolve hunt -p 33 -a "$A" -c t -d "$T/t3" --ui log -q --no-gpu >/dev/null 2>"$T/t3.err"; true)
! grep -q "ThreadSanitizer" "$T/t3.err" && echo "  OK interrupt (TSan)" || { echo "  FAIL interrupt (TSan)"; fail=$((fail+1)); }

run env "$IFMA_ENV" make BUILD=build/tsan OPT="-O1 -fsanitize=thread" lib-test >"$T/tsan-lib.log" 2>&1 \
  && echo "  OK pzcore tests (TSan: first use of the curve table and of the batch hash from several threads)" || { tail -20 "$T/tsan-lib.log"; }
}

# Valgrind on the portable path
sec_valgrind() {
step "Valgrind (memcheck, leaks included; portable path only: Valgrind does not know AVX-512/ADX)"
if command -v valgrind >/dev/null; then
  VG="valgrind -q --error-exitcode=9 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all"
  EPUB=$(epub build/pzsolve)
  K2=0x2aaaaa; A2=$(field $K2 addr)
  rm -rf "$T/v1"; run env PZ_ISA=baseline $VG build/pzsolve hunt -p 22 -a "$A2" -c v -d "$T/v1" -t 2 --no-gpu --ui log -q
  check_found "$T/v1/v" "$K2" && echo "  OK linear (Valgrind)" || { echo "  FAIL linear (Valgrind)"; fail=$((fail+1)); }
  K3=$(python3 -c "import random; random.seed(4); print(hex(random.randrange(2**30, 2**31)))"); P3=$(field "$K3" pub); A3=$(field "$K3" addr)
  rm -rf "$T/v2"; run $VG build/pzsolve hunt -p 31 -a "$A3" -k "$P3" -m kangaroo -c v -d "$T/v2" -t 2 --no-gpu --ui log -q
  check_found "$T/v2/v" "$K3" && echo "  OK kangaroo (Valgrind)" || { echo "  FAIL kangaroo (Valgrind)"; fail=$((fail+1)); }
  rm -rf "$T/v3"; run $VG build/pzsolve hunt -p 31 -a "$A3" -k "$P3" -m bsgs -c v -d "$T/v3" -t 2 --no-gpu --ui log -q
  check_found "$T/v3/v" "$K3" && echo "  OK bsgs (Valgrind)" || { echo "  FAIL bsgs (Valgrind)"; fail=$((fail+1)); }
  rm -rf "$T/v4"; run env PZ_ISA=baseline $VG build/pzsolve hunt -p 22 -a "$A2" -c v -d "$T/v4" -t 2 --no-gpu --ui log -q --encrypt-pubkey "$EPUB" --on-found "true"
  grep -q '^PZENC1:' "$T/v4/v/FOUND.txt" && echo "  OK encrypted key (Valgrind)" || { echo "  FAIL encrypted key (Valgrind)"; fail=$((fail+1)); }
  for t in t_u256 t_address t_hash t_batch t_kdf; do
    run $VG build/pzcore/$t >/dev/null && echo "  OK pzcore $t (Valgrind)"
  done
  PZ_TEST_SEAL_FAST=1 run $VG build/pzcore/t_seal >/dev/null && echo "  OK pzcore t_seal (Valgrind, the key of a password at the real cost left to the other runs)"
else
  echo "  valgrind not installed: skipped"
fi
}

# aarch64 under QEMU
sec_arm64() {
step "aarch64 (cross-compiled, tried under QEMU user-mode emulation: the NEON/SHA-256 code of the library and of the engines)"
if command -v aarch64-linux-gnu-gcc >/dev/null && command -v qemu-aarch64 >/dev/null; then
  ARM=(make CC=aarch64-linux-gnu-gcc BUILD=build/arm64 "LDLIBS=-static -lpthread -lm -ldl" RUN=qemu-aarch64)
  run "${ARM[@]}" -j"$(nproc)" >"$T/arm.log" 2>&1 || tail -20 "$T/arm.log"
  run "${ARM[@]}" test >"$T/arm-test.log" 2>&1 && echo "  OK unit tests, library tests and self-test on aarch64 (QEMU)" || tail -20 "$T/arm-test.log"
  run make CC=aarch64-linux-gnu-gcc BUILD=build/arm64n OPT="-O3 -DPZ_NO_ARM_SHA2" "LDLIBS=-static -lpthread -lm -ldl" RUN=qemu-aarch64 lib-test >"$T/arm-n.log" 2>&1 \
    && echo "  OK library tests on aarch64 without the SHA-256 instructions (portable path)" || tail -20 "$T/arm-n.log"
else
  echo "  aarch64 cross compiler or qemu-aarch64 not installed: skipped"
fi
}

# the Windows exe under Wine
sec_windows() {
step "Windows (static MinGW-w64 exe, tried under Wine)"
unset TMPDIR   # Wine aborts when it is set to a folder other than the one its server started with; the exe uses the temporary folder of Windows
if command -v x86_64-w64-mingw32-gcc-posix >/dev/null && command -v wine >/dev/null; then
  export WINEDEBUG=-all
  run make windows -j"$(nproc)" >/dev/null
  run make wintest >"$T/wintest.log" 2>&1 && echo "  OK unit tests and self-test of the exe" || tail -20 "$T/wintest.log"
  if r=$(tools/check_binary.sh windows build/win/pzsolve.exe 2>&1); then echo "  OK the exe $r"; else echo "  FAIL $r"; fail=$((fail+1)); fi
  run make winmockcuda >/dev/null
  W="$T/win"; rm -rf "$W"; mkdir -p "$W"; cp build/win/pzsolve.exe build/win/mockcuda/nvcuda.dll "$W/"
  (cd "$W" && WINEDLLOVERRIDES=nvcuda=n wine pzsolve.exe selftest 2>&1 | tr -d '\r' | grep -q "Mock CUDA GPU (CPU) via CUDA  *kernels identical to the CPU") && echo "  OK GPU self-test via CUDA (nvcuda.dll)" || { echo "  FAIL GPU self-test via CUDA under Windows"; fail=$((fail+1)); }
  (cd "$W" && wine pzsolve.exe hunt -p 26 -a "$A" -m linear -c w -d camp --no-gpu --ui log -q >/dev/null 2>&1)
  check_found "$W/camp/w" "$K" && echo "  OK linear (exe)" || { echo "  FAIL linear (exe)"; fail=$((fail+1)); }
  (cd "$W" && wine pzsolve.exe hunt -p 44 -a "$KA" -k "$KP" -m bsgs --table t.bin -c b -d camp --no-gpu --ui log -q >/dev/null 2>&1)
  check_found "$W/camp/b" "$KK" && [ -s "$W/t.bin" ] && echo "  OK bsgs with table on file (exe)" || { echo "  FAIL bsgs (exe)"; fail=$((fail+1)); }
  (cd "$W" && WINEDLLOVERRIDES=nvcuda=n wine pzsolve.exe hunt -p 44 -a "$KA" -k "$KP" -m kangaroo -c k -d camp --no-cpu --ui log -q >/dev/null 2>&1)
  check_found "$W/camp/k" "$KK" && echo "  OK kangaroo via CUDA (exe)" || { echo "  FAIL kangaroo via CUDA (exe)"; fail=$((fail+1)); }
  (cd "$W" && wine pzsolve.exe hunt -p 71 -c i -d camp --no-gpu --ui log -q >/dev/null 2>&1 &)
  wait_for 'running "$W/camp/i"'   # started (Wine may take a while) and running: the first report is written 2 s after the start
  P=$(ps -eo pid,args | awk '$2 ~ /pzsolve\.exe$/ && $3=="hunt" {print $1}' | head -1)
  [ -n "$P" ] && { kill -INT "$P"; wait_gone "$P"; }
  [ -s "$W/camp/i/state.dat" ] && grep -q "sessions=1" "$W/camp/i/session.txt" && echo "  OK Ctrl-C: state saved (exe)" || { echo "  FAIL Ctrl-C (exe)"; fail=$((fail+1)); }
else
  echo "  MinGW-w64 or Wine not installed: skipped"
fi
}

# The sections, in the order of the checks when they ran one after the other (-j 1 keeps it). NEEDS: the section that must have ended
# before this one starts (build: build/pzsolve and the simulated driver). WEIGHT: about how many seconds the section takes on a 4-core
# machine when it runs alone (measured: the "==== section" lines); with more than one job the heaviest section that can start goes
# first, so that a long one does not start last.
#  NAME       NEEDS   WEIGHT
SECTIONS="
   sources    -        1
   build      -        160
   o0         -        40
   ptx        -        60
   ifma       build    5
   cuda       build    200
   bench      build    150
   panel      build    120
   machine    build    240
   gpudoctor  build    290
   suspend    build    25
   merge      build    190
   parts      build    15
   e2e        build    300
   asan       -        230
   asanocl    asan     70
   tsan       -        80
   valgrind   build    320
   arm64      -        430
   windows    -        300
"

run_section() {   # NAME, in a subshell: the folders of the section, then its checks; the number of problems goes to ROOT/NAME.fail
  T="$ROOT/$1"; mkdir -p "$T/cache" "$T/tmp"
  export XDG_CACHE_HOME="$T/cache" TMPDIR="$T/tmp"
  fail=0
  "sec_$1"
  echo "$fail" > "$ROOT/$1.fail"
}

declare -A NEEDS WEIGHT PID T0 SUMMARY
PENDING=(); STARTED=(); PROBLEMS=0
while read -r n needs w; do
  [ -n "$n" ] || continue
  if [ "$n" = ptx ] && [ -z "${PZ_NVRTC:-}" ]; then continue; fi    # the PTX check needs NVRTC (docs/en/RELEASING.md)
  PENDING+=("$n"); NEEDS[$n]=$needs; WEIGHT[$n]=$w
done <<<"$SECTIONS"
NSEC=${#PENDING[@]}; ORDER=("${PENDING[@]}")

# the next section to start: one whose NEEDS has ended; with -j 1 the first of the table, otherwise the heaviest
pick() {
  local n best=
  for n in "${PENDING[@]}"; do
    [ "${NEEDS[$n]}" = - ] || [ -e "$ROOT/${NEEDS[$n]}.done" ] || continue
    if [ -z "$best" ]; then best=$n; [ "$JOBS" = 1 ] && break
    elif [ "${WEIGHT[$n]}" -gt "${WEIGHT[$best]}" ]; then best=$n; fi
  done
  echo "$best"
}

# a section has ended: its log is printed whole, then its time
finish() {
  local n=$1 f secs=$((SECONDS - T0[$1])) t
  f=$(cat "$ROOT/$n.fail" 2>/dev/null)
  [ -n "$f" ] || { echo "!!!! FAILED: the section $n stopped before its end" >>"$ROOT/$n.log"; f=1; }
  : >"$ROOT/$n.done"
  PROBLEMS=$((PROBLEMS + f))
  t=$(printf '%d:%02d' $((secs / 60)) $((secs % 60)))
  cat "$ROOT/$n.log"
  if [ "$f" = 0 ]; then echo "==== section $n: passed in $t"; else echo "==== section $n: $f problems, in $t"; fi
  SUMMARY[$n]=$(printf '  %-10s %6s  %s' "$n" "$t" "$([ "$f" = 0 ] && echo passed || echo "$f problems")")
}

echo "verify: $NSEC sections, up to $JOBS at a time${QUICK:+ (quick)}; the output of each one comes when it ends"
make clean >/dev/null || { echo "verify: make clean failed"; exit 2; }   # build/ from scratch: the sections build in it, or below it
while [ ${#PENDING[@]} -gt 0 ] || [ ${#STARTED[@]} -gt 0 ]; do
  while [ ${#STARTED[@]} -lt "$JOBS" ]; do
    n=$(pick)
    [ -n "$n" ] || break
    ( run_section "$n" ) >"$ROOT/$n.log" 2>&1 </dev/null &
    PID[$n]=$!; T0[$n]=$SECONDS; STARTED+=("$n")
    rest=(); for p in "${PENDING[@]}"; do [ "$p" = "$n" ] || rest+=("$p"); done; PENDING=(${rest[@]+"${rest[@]}"})
  done
  RUNNING=(); for n in ${STARTED[@]+"${STARTED[@]}"}; do RUNNING+=("${PID[$n]}"); done
  if [ ${#STARTED[@]} = 0 ]; then echo "verify: no section can start (NEEDS names a section that is not in the table)"; PROBLEMS=$((PROBLEMS + 1)); break; fi
  wait -n 2>/dev/null   # until a section ends
  rest=()
  for n in "${STARTED[@]}"; do
    if kill -0 "${PID[$n]}" 2>/dev/null; then rest+=("$n"); else wait "${PID[$n]}" 2>/dev/null; finish "$n"; fi
  done
  STARTED=(${rest[@]+"${rest[@]}"})
done
RUNNING=()

OKS=$(cat "$ROOT"/*.log | grep -c -E '^ +OK ')
FAILS=$(cat "$ROOT"/*.log | grep -c -E '^ +FAIL |^!!!! FAILED')
echo
echo "######## summary: $NSEC sections in $((SECONDS / 60)):$(printf '%02d' $((SECONDS % 60))) (-j $JOBS${QUICK:+, quick}): $OKS OK, $FAILS FAIL"
for n in "${ORDER[@]}"; do [ -n "${SUMMARY[$n]:-}" ] && echo "${SUMMARY[$n]}"; done
echo
if [ $PROBLEMS -eq 0 ]; then echo "FULL VERIFICATION PASSED"; else echo "VERIFICATION FAILED: $PROBLEMS problems"; fi
exit $((PROBLEMS > 255 ? 255 : PROBLEMS))
