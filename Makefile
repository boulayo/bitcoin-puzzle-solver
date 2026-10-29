# SPDX-License-Identifier: AGPL-3.0-or-later
# pzsolve - Makefile
# Targets: all (default), test, test-shani, asan, tsan, windows, wintest, clean
#   make windows   cross-compiles build/win/pzsolve.exe with MinGW-w64 (static, no DLLs needed)
#   make wintest   builds the Windows unit tests and runs them (and the self-test) under Wine
#   make test-shani   runs the library tests, the shani engine test and the self-test under qemu-x86_64 -cpu max, which executes the SHA
#                  extensions on any x86-64 Linux machine (skipped when qemu-x86_64 is not installed; see the rule below)
#
# Platforms: Linux x86-64 and arm64 (gcc or clang), macOS arm64 and x86-64 (Apple clang), Windows x86-64 (MinGW-w64).
#   Build:  make -j"$(nproc)"                  (Linux)       make -j"$(sysctl -n hw.ncpu)"   (macOS)
#   Check:  build/pzsolve selftest
# Release builds are portable: no -march/-mcpu flag is passed (the program picks its SIMD engine at run time). Options:
#   ARCH_NATIVE=1   tune everything for the build machine (-march=native); the binary may not run on other CPUs
#   X86=0           force the portable scalar code even on an x86-64 machine (what arm64 builds use)
#   CC=clang        use another compiler (the default is `cc`: gcc on Linux, Apple clang on macOS)
#   MACOS_MIN=11.0  oldest macOS the binary will run on

# the first rule of the file would be the default goal: say which one it is (lib/pzcore adds rules above `all`)
.DEFAULT_GOAL := all

# a rule whose recipe fails does not leave its target behind: a generated file that embed (or the compiler) wrote only in part would be
# newer than its inputs, and the next make would take it as up to date and fail on it until `make clean`. Only the targets of the rules
# are affected (the objects, the archive, the generated sources, the test programs, the mock driver); the phony ones such as `test` stay.
.DELETE_ON_ERROR:

VERSION  := $(shell cat VERSION)
GITREV   := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)
BUILDINFO := git-$(GITREV)/$(shell $(CC) -dumpmachine)

HOSTCC  ?= cc
OPT     ?= -O3
WARN    := -Wall -Wextra -Wshadow -Wno-unused-parameter
COMMON  := -std=gnu11 $(OPT) -g $(WARN) -D_GNU_SOURCE -fno-strict-aliasing \
           -DPZ_VERSION=\"$(VERSION)\" -DPZ_BUILD_INFO=\"$(BUILDINFO)\" -Isrc -Ilib/pzcore/include
LDLIBS  := -lpthread -lm -ldl

# clang (including Apple clang): without the SLP vectorizer. Apple clang 17 (x86-64, -O3) miscompiled the AVX2 lane step of the
# kangaroo (the built-in self-test caught it: the result differed from the reference; -O0..-O2 and -O3 -fno-slp-vectorize are right).
# The SIMD code uses explicit vector arithmetic, so the SLP pass has nothing to gain there.
# Only the first line of the version output is looked at: a later one can mention clang too (an InstalledDir line for a path with
# clang in it) and would make the count 2.
ISCLANG := $(shell $(CC) --version 2>/dev/null | head -n1 | grep -ci clang)
ifeq ($(ISCLANG),1)
COMMON  += -fno-slp-vectorize
endif

# macOS (Apple clang): pthreads, dlopen and the math library all live in libSystem, so only -lm is needed (and -pthread would
# only draw an "unused argument" warning); no static linking (not supported); the deployment target sets how old a macOS the
# binary runs on (11.0 is the first release for Apple Silicon and has every API the program uses).
DARWIN := $(shell $(CC) -dumpmachine | grep -c apple-darwin)
ifeq ($(DARWIN),1)
MACOS_MIN ?= 11.0
DEPLOY  := -mmacosx-version-min=$(MACOS_MIN)
COMMON  += $(DEPLOY)
LDLIBS  := -lm
else
DEPLOY  :=
COMMON  += -pthread
endif

# Windows (MinGW-w64): C99 printf (%Lf, %zu), 64-bit file offsets, unaligned vector moves (GCC cannot keep the Win64
# stack 32/64-byte aligned for AVX spills), static runtime, 8 MiB thread stacks like Linux.
WIN := $(shell $(CC) -dumpmachine | grep -c mingw)
ifeq ($(WIN),1)
EXE     := .exe
COMMON  += -D__USE_MINGW_ANSI_STDIO=1 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 -Wa,-muse-unaligned-vector-move
LDLIBS  := -static -Wl,--stack,8388608 -lpthread -lbcrypt -lm
RUN     ?= wine
endif

BUILD   ?= build
BIN     := $(BUILD)/pzsolve$(EXE)

CORE_SRC := platform envopts util lz campaign campaigns puzzles puzzles_data eta ui bsgs jobq cpu_common sysinfo run_common run_linear run_kangaroo run_bsgs bench_tg profile doctor selftest main
ENG_BASE := src/eng_base.c
ENG_AVX2 := src/eng_avx2.c
ENG_AVX5 := src/eng_avx512.c
KERNELS  := src/kernels/pz_common.cl src/kernels/pz_linear.cl src/kernels/pz_kang.h src/kernels/pz_kang.cl src/kernels/pz_test.cl
# CUDA modules: PTX generated from the same kernels by tools/gen_ptx.py (committed, so building needs no CUDA toolkit). Their list is
# src/kernels/ptx/modules.txt, the only one: the lines are read here (through tools/ptx_table.awk), by gen_ptx.py, by the CUDA runtime
# (build/ptx_mods.c is made from it) and by the simulated driver of tests/mock_cuda.
PTXTAB   := src/kernels/ptx/modules.txt
PTXAWK   := awk -f tools/ptx_table.awk
PTXMODS  := $(shell $(PTXAWK) -v mode=names $(PTXTAB))
PTX      := $(addprefix src/kernels/ptx/,$(addsuffix .ptx,$(PTXMODS)))
# The PTX modules are embedded compressed (src/lz.c, tools/embed.c -z) and unpacked when one is loaded. The modules of a family differ
# in few lines (linear128 and linear256 in 148 of 38,000), so each one is compressed against another, its base, whose text serves as
# the dictionary: all together they take about 1 MB instead of 12 MB. Name:base pairs from the table; a base must come before its
# dependents there, and a module without a base is compressed on its own.
PTXBASE  := $(shell $(PTXAWK) -v mode=bases $(PTXTAB))
PTXITEMS := $(foreach p,$(PTXMODS),pz_ptx_$(p)$(addprefix :pz_ptx_,$(patsubst $(p):%,%,$(filter $(p):%,$(PTXBASE)))) src/kernels/ptx/$(p).ptx)
KANG_SRC := src/kangaroo.c src/kang_log.c src/kang_cpu.c src/kang_selftest.c src/kang_base.c src/bsgs_base.c

GPU_SRC  := cl_api rt rt_cl rt_cuda gpu gpu_tune gpu_selftest
SRCS   := $(addprefix src/,$(addsuffix .c,$(CORE_SRC) $(GPU_SRC))) $(KANG_SRC) $(BUILD)/kernels_gen.c $(BUILD)/ptx_gen.c $(BUILD)/ptx_mods.c
OBJS   := $(patsubst $(BUILD)/%.c,$(BUILD)/%.o,$(patsubst src/%.c,$(BUILD)/%.o,$(SRCS)))
ENGOBJ := $(BUILD)/eng_base.o $(BUILD)/eng_avx2.o $(BUILD)/eng_shani.o $(BUILD)/eng_avx512.o $(BUILD)/eng_avx2v.o $(BUILD)/eng_avx512v.o $(BUILD)/eng_avx512i.o
KVOBJ  := $(BUILD)/kang_x86.o $(BUILD)/kang_avx2v.o $(BUILD)/kang_avx512v.o $(BUILD)/kang_avx512i.o $(BUILD)/kang_avx512i2.o
BSVOBJ := $(BUILD)/bsgs_x86.o $(BUILD)/bsgs_avx2v.o $(BUILD)/bsgs_avx512v.o $(BUILD)/bsgs_avx512i.o
DEPS   := $(OBJS:.o=.d) $(ENGOBJ:.o=.d) $(KVOBJ:.o=.d) $(BSVOBJ:.o=.d)

# X86=1 builds the per-ISA SIMD engines (each object file with its own -m flags, picked at run time by CPU detection); on any other
# CPU only the portable engine is built and -DPZ_NO_X86_VARIANTS removes the rest. On arm64 (Apple Silicon, Linux aarch64) ARM64=1 adds
# the neon and neon2 engines, built with -march=armv8-a+crypto and chosen at run time when the CPU has the SHA-256 instructions.
# The -m flags below are used only when X86=1: nothing x86-specific reaches an arm64 compiler.
X86 := $(shell $(CC) -dumpmachine | grep -cE 'x86_64|amd64')
ARM64 := $(shell $(CC) -dumpmachine | grep -cE 'aarch64|arm64')
ifeq ($(ARM64),1)
NEON_FLAGS   := -march=armv8-a+crypto
endif
ifeq ($(X86),1)
AVX2_FLAGS   := -mavx2 -mbmi2 -madx
AVX512_FLAGS := -mavx512f -mavx512vl -mavx512bw -mavx512dq -mbmi2 -madx
IFMA_FLAGS   := $(AVX512_FLAGS) -mavx512ifma
SHANI_FLAGS  := -msha -mavx2 -mssse3 -msse4.1
else
COMMON += -DPZ_NO_X86_VARIANTS
endif

ARCH_NATIVE ?= 0
ifeq ($(ARCH_NATIVE),1)
ifeq ($(X86),1)
COMMON += -march=native
else
COMMON += -mcpu=native
endif
endif

# ---- pzcore: the reusable core (lib/pzcore) ----
# Built into build/libpzcore.a from the sources of lib/pzcore alone: no -Isrc, so nothing of pzsolve can leak in, and with the
# strict-aliasing rules on (the rest of pzsolve is built with -fno-strict-aliasing). pzsolve and its unit tests link the archive
# (so there is a single copy of the code); the search engines include the headers and templates of lib/pzcore/include and
# are compiled for their own instruction sets, as before. The per-ISA objects follow the same X86 / ARM64 switches as the engines.
LIBFLAGS  = $(filter-out -fno-strict-aliasing -Isrc,$(COMMON))
LIBLDLIBS = $(filter-out -ldl -lbcrypt,$(LDLIBS))
PZC_SRC   := u256 fe ec hash address perm cpu hash160_batch h160_portable
ifeq ($(X86),1)
PZC_SRC   += h160_avx2 h160_avx512 h160_shani
endif
ifeq ($(ARM64),1)
PZC_SRC   += h160_neon h160_neon2 sha256_arm
endif
LIBOBJ    := $(addprefix $(BUILD)/pzcore/,$(addsuffix .o,$(PZC_SRC)))
LIB       := $(BUILD)/libpzcore.a
DEPS      += $(LIBOBJ:.o=.d)

$(BUILD)/pzcore:
	@mkdir -p $@

# the objects of the library for one instruction set get its flags from the target-specific ISAFLAGS (empty for the others)
$(BUILD)/pzcore/%.o: lib/pzcore/src/%.c | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS)$(if $(ISAFLAGS), $(ISAFLAGS)) -MMD -MP -c $< -o $@
$(BUILD)/pzcore/h160_avx2.o: ISAFLAGS := $(AVX2_FLAGS)
$(BUILD)/pzcore/h160_avx512.o: ISAFLAGS := $(AVX512_FLAGS)
$(BUILD)/pzcore/h160_shani.o: ISAFLAGS := $(SHANI_FLAGS)
$(BUILD)/pzcore/h160_neon.o $(BUILD)/pzcore/h160_neon2.o $(BUILD)/pzcore/sha256_arm.o: ISAFLAGS := $(NEON_FLAGS)

$(LIB): $(LIBOBJ)
	rm -f $@
	$(AR) rcs $@ $^

lib: $(LIB)

# the unit tests and the examples of the library link nothing but the archive
LIBTESTS := t_u256 t_fe t_ec t_hash t_address t_perm t_batch
ifeq ($(X86),1)
LIBTESTS += t_fe_asm t_fev2 t_fev5 t_fev52
endif
LIBEXAMPLES := keyaddr roundtrip permute batch8

$(BUILD)/pzcore/t_fe_asm$(EXE): lib/pzcore/tests/t_fe.c $(LIB) | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS) $(AVX2_FLAGS) -o $@ $< $(LIB) $(LIBLDLIBS)
$(BUILD)/pzcore/t_fev2$(EXE): lib/pzcore/tests/t_fev.c $(LIB) | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS) $(AVX2_FLAGS) -DPZ_VBYTES=32 -o $@ $< $(LIB) $(LIBLDLIBS)
$(BUILD)/pzcore/t_fev5$(EXE): lib/pzcore/tests/t_fev.c $(LIB) | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS) $(AVX512_FLAGS) -DPZ_VBYTES=64 -o $@ $< $(LIB) $(LIBLDLIBS)
$(BUILD)/pzcore/t_fev52$(EXE): lib/pzcore/tests/t_fev52.c $(LIB) | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS) $(IFMA_FLAGS) -o $@ $< $(LIB) $(LIBLDLIBS)
$(BUILD)/pzcore/t_%$(EXE): lib/pzcore/tests/t_%.c lib/pzcore/tests/testutil.h $(LIB) | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS) -o $@ $< $(LIB) $(LIBLDLIBS)
$(BUILD)/pzcore/ex_%$(EXE): lib/pzcore/examples/%.c $(LIB) | $(BUILD)/pzcore
	$(CC) $(LIBFLAGS) -o $@ $< $(LIB) $(LIBLDLIBS)

lib-test: $(addsuffix $(EXE),$(addprefix $(BUILD)/pzcore/,$(LIBTESTS)))
	@for t in $(LIBTESTS); do $(RUN) ./$(BUILD)/pzcore/$$t$(EXE) || exit 1; done

# the public headers as C++ (the library is C, but is meant to be called from C++ too)
CXX ?= c++
lib-cxx: $(LIB) lib/pzcore/tests/cxx_check.cpp
	$(CXX) -std=c++14 -Wall -Wextra -Wpedantic $(DEPLOY) -pthread -Ilib/pzcore/include -o $(BUILD)/pzcore/cxx_check$(EXE) lib/pzcore/tests/cxx_check.cpp $(LIB) $(LIBLDLIBS)
	$(RUN) ./$(BUILD)/pzcore/cxx_check$(EXE)

lib-examples: $(addsuffix $(EXE),$(addprefix $(BUILD)/pzcore/ex_,$(LIBEXAMPLES)))
	@for e in $(LIBEXAMPLES); do echo "== $$e"; $(RUN) ./$(BUILD)/pzcore/ex_$$e$(EXE) || exit 1; done

.PHONY: all lib lib-test lib-cxx lib-examples test test-shani e2e asan tsan clean windows wintest mockcuda winmockcuda

all: $(BIN)

$(BUILD):
	@mkdir -p $(BUILD)

# PZ_VERSION and PZ_BUILD_INFO reach the compiler as -D options, which make does not see: a new VERSION or a new commit changed no
# dependency, and an incremental build went on reporting the old ones. The objects whose sources use the macros depend on a stamp file
# named after the version, the commit and the target. When any of the three changes, the stamp is a new file (the old one is removed),
# newer than those objects, and they are compiled again; while they stay the same nothing is compiled, and `make -n` is accurate.
# A header that used the macros would need its includers listed here.
VSTAMP       := $(BUILD)/version-$(subst /,_,$(VERSION)-$(BUILDINFO)).stamp
VERSION_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(shell grep -l -E 'PZ_VERSION|PZ_BUILD_INFO' src/*.c))
$(VERSION_OBJS): $(VSTAMP)
$(VSTAMP): | $(BUILD)
	@rm -f $(BUILD)/version-*.stamp; touch $@

# embed runs on the build machine; it holds the encoder of src/lz.c's format and checks every stream it writes with the decoder
$(BUILD)/embed: tools/embed.c src/lz.c src/lz.h | $(BUILD)
	$(HOSTCC) -O2 -Isrc -o $@ tools/embed.c src/lz.c

$(BUILD)/kernels_gen.c: $(KERNELS) $(BUILD)/embed
	$(BUILD)/embed $@ pz_common_cl src/kernels/pz_common.cl pz_linear_cl src/kernels/pz_linear.cl pz_kang_h src/kernels/pz_kang.h pz_kang_cl src/kernels/pz_kang.cl pz_test_cl src/kernels/pz_test.cl

$(BUILD)/kernels_gen.o: $(BUILD)/kernels_gen.c
	$(CC) $(DEPLOY) -O1 -c $< -o $@

$(BUILD)/ptx_gen.c: $(PTXTAB) $(PTX) $(BUILD)/embed
	$(BUILD)/embed -z $@ $(PTXITEMS)

$(BUILD)/ptx_gen.o: $(BUILD)/ptx_gen.c src/lz.h
	$(CC) $(DEPLOY) -O1 -Isrc -c $< -o $@

# which module the CUDA runtime loads for each program and parameter of rt_build
$(BUILD)/ptx_mods.c: $(PTXTAB) tools/ptx_table.awk | $(BUILD)
	$(PTXAWK) -v mode=rtcu $(PTXTAB) > $@

$(BUILD)/ptx_mods.o: $(BUILD)/ptx_mods.c src/rt_priv.h
	$(CC) $(COMMON) -MMD -MP -c $< -o $@

# the list of the embedded modules and of their files, for tests/t_kernels.c
$(BUILD)/ptx_tab.c: $(PTXTAB) $(PTX) $(BUILD)/embed
	$(BUILD)/embed -T $@ $(PTXITEMS)

$(BUILD)/ptx_tab.o: $(BUILD)/ptx_tab.c src/lz.h
	$(CC) $(DEPLOY) -O1 -Isrc -c $< -o $@

# The objects of the engines for one instruction set (eng_*, kang_*, bsgs_*) are built by this rule with its flags, in the target-specific ISAFLAGS;
# the rest, eng_base included, with none.
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(COMMON)$(if $(ISAFLAGS), $(ISAFLAGS)) -MMD -MP -c $< -o $@

OBJS_NEON   := $(addprefix $(BUILD)/,$(addsuffix .o,eng_neon eng_neon2))
OBJS_AVX2   := $(addprefix $(BUILD)/,$(addsuffix .o,eng_avx2 eng_avx2v kang_x86 kang_avx2v bsgs_x86 bsgs_avx2v))
OBJS_AVX512 := $(addprefix $(BUILD)/,$(addsuffix .o,eng_avx512 eng_avx512v kang_avx512v bsgs_avx512v))
OBJS_IFMA   := $(addprefix $(BUILD)/,$(addsuffix .o,eng_avx512i kang_avx512i kang_avx512i2 bsgs_avx512i))
$(OBJS_NEON): ISAFLAGS := $(NEON_FLAGS)
$(OBJS_AVX2): ISAFLAGS := $(AVX2_FLAGS)
$(OBJS_AVX512): ISAFLAGS := $(AVX512_FLAGS)
$(OBJS_IFMA): ISAFLAGS := $(IFMA_FLAGS)
$(BUILD)/eng_shani.o: ISAFLAGS := $(AVX2_FLAGS) $(SHANI_FLAGS)

ifeq ($(X86),1)
ALLENG := $(ENGOBJ) $(KVOBJ) $(BSVOBJ)
TESTENG := $(ENGOBJ) $(BSVOBJ) $(LIB)       # the unit tests do not link the kangaroo
else
ALLENG := $(BUILD)/eng_base.o
ifeq ($(ARM64),1)
ALLENG += $(BUILD)/eng_neon.o $(BUILD)/eng_neon2.o
DEPS   += $(OBJS_NEON:.o=.d)   # a change to a header they include rebuilds them
endif
TESTENG := $(ALLENG) $(LIB)
endif

$(BIN): $(OBJS) $(ALLENG) $(LIB)
	$(CC) $(COMMON) -o $@ $^ $(LDLIBS)

# Unit tests (built against the same sources)
TESTS := t_core t_env t_jobq t_engine t_puzzles t_eta t_bsgs t_fuzz t_ui t_gpufe t_lz t_kernels
TESTOBJS := $(filter-out $(BUILD)/main.o $(BUILD)/selftest.o $(BUILD)/run_common.o $(BUILD)/run_linear.o $(BUILD)/run_kangaroo.o $(BUILD)/bench_tg.o $(BUILD)/profile.o $(BUILD)/doctor.o $(BUILD)/gpu.o $(BUILD)/gpu_tune.o $(BUILD)/gpu_selftest.o $(BUILD)/rt.o $(BUILD)/rt_cl.o $(BUILD)/rt_cuda.o $(BUILD)/cl_api.o $(BUILD)/kernels_gen.o $(BUILD)/ptx_gen.o $(BUILD)/ptx_mods.o $(BUILD)/kangaroo.o $(BUILD)/kang_log.o $(BUILD)/kang_cpu.o $(BUILD)/kang_selftest.o $(BUILD)/kang_base.o $(BUILD)/run_bsgs.o,$(OBJS))

# t_gpufe: the kernel field arithmetic compiled for the host: PTX carry-chain variant (emulated), the OpenCL one with add-with-carry
# (builtins of the compiler or their C equivalent) and the OpenCL one in 64-bit C
$(BUILD)/kfe_fast.o: tests/mock_cuda/kern_fe.c tests/mock_cuda/pz_host.h src/kernels/pz_common.cl | $(BUILD)
	$(CC) $(DEPLOY) -std=gnu11 -O2 -w -Itests/mock_cuda -DPZ_FAST_FE -DGK=gf_ -c $< -o $@
$(BUILD)/kfe_carry.o: tests/mock_cuda/kern_fe.c tests/mock_cuda/pz_host.h src/kernels/pz_common.cl | $(BUILD)
	$(CC) $(DEPLOY) -std=gnu11 -O2 -w -Itests/mock_cuda -DPZ_TEST_CARRY -DGK=gc_ -Dpz_mock_gid=pz_mock_gid3 -c $< -o $@
$(BUILD)/kfe_port.o: tests/mock_cuda/kern_fe.c tests/mock_cuda/pz_host.h src/kernels/pz_common.cl | $(BUILD)
	$(CC) $(DEPLOY) -std=gnu11 -O2 -w -Itests/mock_cuda -DPZ_TEST_NO_BUILTINS -DGK=gp_ -Dpz_mock_gid=pz_mock_gid2 -c $< -o $@
$(BUILD)/t_gpufe$(EXE): tests/t_gpufe.c $(BUILD)/kfe_fast.o $(BUILD)/kfe_carry.o $(BUILD)/kfe_port.o $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) -o $@ $< $(BUILD)/kfe_fast.o $(BUILD)/kfe_carry.o $(BUILD)/kfe_port.o $(TESTOBJS) $(TESTENG) $(LDLIBS)

# t_lz tests the decoder and the encoder (tools/embed.c, which it includes); t_kernels checks that the modules embedded in the program
# unpack to the files of src/kernels/ptx, which is what the simulated CUDA driver compares them with
$(BUILD)/t_lz$(EXE): tests/t_lz.c tools/embed.c src/lz.h $(BUILD)/lz.o | $(BUILD)
	$(CC) $(COMMON) -o $@ $< $(BUILD)/lz.o $(LDLIBS)
$(BUILD)/t_kernels$(EXE): tests/t_kernels.c src/lz.h $(BUILD)/lz.o $(BUILD)/ptx_gen.o $(BUILD)/ptx_tab.o | $(BUILD)
	$(CC) $(COMMON) -o $@ $< $(BUILD)/lz.o $(BUILD)/ptx_gen.o $(BUILD)/ptx_tab.o $(LDLIBS)

$(BUILD)/t_%$(EXE): tests/t_%.c $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) -o $@ $< $(TESTOBJS) $(TESTENG) $(LDLIBS)

test: lib-test $(BIN) $(addsuffix $(EXE),$(addprefix $(BUILD)/,$(TESTS)))
	@for t in $(TESTS); do $(RUN) ./$(BUILD)/$$t$(EXE) || exit 1; done
	$(RUN) ./$(BIN) selftest

# test-shani: the SHA-extension code (the "shani" kernel of the library and the "shani" engine) runs only on a CPU that has the extensions (AMD
# Zen 2 and later, Intel Ice Lake and later). QEMU's user-mode emulator executes them on any x86-64 Linux machine, so this target runs the
# library tests, the shani engine test and the self-test under `qemu-x86_64 -cpu max`, and is skipped when that is not installed. With
# PZ_EXPECT_SHANI=1 the tests fail if the emulated CPU does not report the extensions, so the shani paths cannot be skipped by accident.
# The emulated CPU has no AVX-512, so the other engines are the AVX2 ones. The same by hand, for a build in build/:
#   PZ_EXPECT_SHANI=1 make RUN="qemu-x86_64 -cpu max" lib-test
#   PZ_EXPECT_SHANI=1 qemu-x86_64 -cpu max build/t_engine shani
#   PZ_EXPECT_SHANI=1 qemu-x86_64 -cpu max build/pzsolve selftest
QEMU_X86 ?= qemu-x86_64
ifeq ($(X86)$(WIN)$(DARWIN),100)
test-shani: $(addsuffix $(EXE),$(addprefix $(BUILD)/pzcore/,$(LIBTESTS))) $(BIN) $(BUILD)/t_engine$(EXE)
	@if ! command -v $(QEMU_X86) >/dev/null 2>&1; then echo "test-shani: $(QEMU_X86) is not installed: skipped"; exit 0; fi; \
	export PZ_EXPECT_SHANI=1; Q="$(QEMU_X86) -cpu max"; \
	for t in $(LIBTESTS); do $$Q ./$(BUILD)/pzcore/$$t || exit 1; done; \
	$$Q ./$(BUILD)/t_engine shani || exit 1; \
	$$Q ./$(BIN) selftest >$(BUILD)/selftest-shani.log 2>&1 || { tail -n 30 $(BUILD)/selftest-shani.log; exit 1; }; \
	grep -q 'CPU engine shani  *OK' $(BUILD)/selftest-shani.log || { echo "test-shani: the self-test did not run the shani engine"; exit 1; }; \
	tail -n 1 $(BUILD)/selftest-shani.log; echo "test-shani: OK"
else
test-shani:
	@echo "test-shani: only for a Linux x86-64 build: skipped"
endif

WINCC ?= x86_64-w64-mingw32-gcc-posix
windows:
	$(MAKE) CC=$(WINCC) BUILD=build/win all

wintest:
	$(MAKE) CC=$(WINCC) BUILD=build/win test

e2e: $(BIN)
	tests/e2e.sh $(BIN)

asan:
	$(MAKE) BUILD=build/asan OPT="-O1 -fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer" all

tsan:
	$(MAKE) BUILD=build/tsan OPT="-O1 -fsanitize=thread" all

# Mock CUDA driver for tests (libcuda.so.1 / nvcuda.dll): runs the kernel sources on the CPU behind the driver API,
# so the whole CUDA code path can be exercised without an NVIDIA GPU (LD_LIBRARY_PATH / WINEDLLPATH, see tools/verify.sh).
MOCK    := $(BUILD)/mockcuda
ifeq ($(WIN),1)
MOCKLIB := $(MOCK)/nvcuda.dll
MOCKLD  := -shared -static
else
MOCKLIB := $(MOCK)/libcuda.so.1
MOCKLD  := -shared -fPIC
endif
MOCKCC  := $(CC) -std=gnu11 -O2 -fPIC -w -Itests/mock_cuda -DPZ_FAST_FE
MOCKSRC := tests/mock_cuda/mock_cuda.c tests/mock_cuda/kern.c tests/mock_cuda/pz_host.h $(KERNELS)

$(MOCK)/ptx_gen.c: $(PTXTAB) $(PTX) $(BUILD)/embed
	@mkdir -p $(MOCK)
	$(BUILD)/embed $@ $(foreach p,$(PTXMODS),mock_ptx_$(p) src/kernels/ptx/$(p).ptx)

# the modules the simulated driver knows and the kernel it runs for each (included by mock_cuda.c), and the objects of those kernels
$(MOCK)/mock_mods.h: $(PTXTAB) tools/ptx_table.awk
	@mkdir -p $(MOCK)
	$(PTXAWK) -v mode=mockhdr $(PTXTAB) > $@

MOCKOBJS := $(shell $(PTXAWK) -v mode=mockobjs -v dir=$(MOCK) $(PTXTAB))

$(MOCKLIB): $(MOCKSRC) $(MOCK)/ptx_gen.c $(MOCK)/mock_mods.h
	$(shell $(PTXAWK) -v mode=mockcc -v "cc=$(MOCKCC)" -v dir=$(MOCK) $(PTXTAB))
	$(MOCKCC) -c tests/mock_cuda/kern.c -DMOCK_TEST -o $(MOCK)/kt.o
	$(MOCKCC) -c $(MOCK)/ptx_gen.c -o $(MOCK)/ptx_gen.o
	$(MOCKCC) $(MOCKLD) -I$(MOCK) -o $@ tests/mock_cuda/mock_cuda.c $(MOCKOBJS) $(MOCK)/kt.o $(MOCK)/ptx_gen.o

mockcuda: $(MOCKLIB)

winmockcuda:
	$(MAKE) CC=$(WINCC) BUILD=build/win mockcuda

clean:
	rm -rf build

-include $(DEPS)
