# SPDX-License-Identifier: AGPL-3.0-or-later
# pzsolve - Makefile
# Targets: all (default), test, asan, tsan, windows, wintest, clean
#   make windows   cross-compiles build/win/pzsolve.exe with MinGW-w64 (static, no DLLs needed)
#   make wintest   builds the Windows unit tests and runs them (and the self-test) under Wine
#
# Platforms: Linux x86-64 and arm64 (gcc or clang), macOS arm64 and x86-64 (Apple clang), Windows x86-64 (MinGW-w64).
#   Build:  make -j"$(nproc)"                  (Linux)       make -j"$(sysctl -n hw.ncpu)"   (macOS)
#   Check:  build/pzsolve selftest
# Release builds are portable: no -march/-mcpu flag is passed (the program picks its SIMD engine at run time). Options:
#   ARCH_NATIVE=1   tune everything for the build machine (-march=native); the binary may not run on other CPUs
#   X86=0           force the portable scalar code even on an x86-64 machine (what arm64 builds use)
#   CC=clang        use another compiler (the default is `cc`: gcc on Linux, Apple clang on macOS)
#   MACOS_MIN=11.0  oldest macOS the binary will run on
VERSION  := $(shell cat VERSION)
GITREV   := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)
BUILDINFO := git-$(GITREV)/$(shell $(CC) -dumpmachine)

HOSTCC  ?= cc
OPT     ?= -O3
WARN    := -Wall -Wextra -Wshadow -Wno-unused-parameter
COMMON  := -std=gnu11 $(OPT) -g $(WARN) -D_GNU_SOURCE -fno-strict-aliasing \
           -DPZ_VERSION=\"$(VERSION)\" -DPZ_BUILD_INFO=\"$(BUILDINFO)\" -Isrc
LDLIBS  := -lpthread -lm -ldl

# clang (including Apple clang): without the SLP vectorizer. Apple clang 17 (x86-64, -O3) miscompiled the AVX2 lane step of the
# kangaroo (the built-in self-test caught it: the result differed from the reference; -O0..-O2 and -O3 -fno-slp-vectorize are right).
# The SIMD code uses explicit vector arithmetic, so the SLP pass has nothing to gain there.
ISCLANG := $(shell $(CC) --version 2>/dev/null | grep -ci clang)
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

CORE_SRC := platform util u256 ec hash perm campaign campaigns puzzles puzzles_data eta ui bsgs jobq cpu_common cpu_arm sysinfo run_common run_linear run_kangaroo run_bsgs bench_tg profile doctor selftest main
ENG_BASE := src/eng_base.c
ENG_AVX2 := src/eng_avx2.c
ENG_AVX5 := src/eng_avx512.c
KERNELS  := src/kernels/pz_common.cl src/kernels/pz_linear.cl src/kernels/pz_kang.cl src/kernels/pz_test.cl
# CUDA modules: PTX generated from the same kernels by tools/gen_ptx.py (committed, so building needs no CUDA toolkit)
PTXMODS  := linear128 linear256 linear512 linear128m4 linear256m4 linear512m4 kang64 kang128 kang256 kang64b kang128b kang256b kang16 kang32 kang16b kang32b kang64c kang128c test
PTX      := $(addprefix src/kernels/ptx/,$(addsuffix .ptx,$(PTXMODS)))
KANG_SRC := src/kangaroo.c src/kang_base.c src/bsgs_base.c

GPU_SRC  := cl_api rt rt_cl rt_cuda gpu
SRCS   := $(addprefix src/,$(addsuffix .c,$(CORE_SRC) $(GPU_SRC))) $(KANG_SRC) $(BUILD)/kernels_gen.c
OBJS   := $(patsubst $(BUILD)/%.c,$(BUILD)/%.o,$(patsubst src/%.c,$(BUILD)/%.o,$(SRCS)))
ENGOBJ := $(BUILD)/eng_base.o $(BUILD)/eng_avx2.o $(BUILD)/eng_avx512.o $(BUILD)/eng_avx2v.o $(BUILD)/eng_avx512v.o $(BUILD)/eng_avx512i.o
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

.PHONY: all test e2e asan tsan clean windows wintest mockcuda winmockcuda

all: $(BIN)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/embed: tools/embed.c | $(BUILD)
	$(HOSTCC) -O1 -o $@ $<

$(BUILD)/kernels_gen.c: $(KERNELS) $(PTX) $(BUILD)/embed
	$(BUILD)/embed $@ pz_common_cl src/kernels/pz_common.cl pz_linear_cl src/kernels/pz_linear.cl pz_kang_cl src/kernels/pz_kang.cl pz_test_cl src/kernels/pz_test.cl \
	  $(foreach p,$(PTXMODS),pz_ptx_$(p) src/kernels/ptx/$(p).ptx)

$(BUILD)/kernels_gen.o: $(BUILD)/kernels_gen.c
	$(CC) $(DEPLOY) -O1 -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(COMMON) -MMD -MP -c $< -o $@

$(BUILD)/eng_base.o: src/eng_base.c | $(BUILD)
	$(CC) $(COMMON) -MMD -MP -c $< -o $@
$(BUILD)/eng_neon.o: src/eng_neon.c | $(BUILD)
	$(CC) $(COMMON) $(NEON_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/eng_neon2.o: src/eng_neon2.c | $(BUILD)
	$(CC) $(COMMON) $(NEON_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/eng_avx2.o: src/eng_avx2.c | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/eng_avx512.o: src/eng_avx512.c | $(BUILD)
	$(CC) $(COMMON) $(AVX512_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/eng_avx2v.o: src/eng_avx2v.c | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/eng_avx512v.o: src/eng_avx512v.c | $(BUILD)
	$(CC) $(COMMON) $(AVX512_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/eng_avx512i.o: src/eng_avx512i.c | $(BUILD)
	$(CC) $(COMMON) $(IFMA_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/kang_x86.o: src/kang_x86.c | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/kang_avx2v.o: src/kang_avx2v.c | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/kang_avx512v.o: src/kang_avx512v.c | $(BUILD)
	$(CC) $(COMMON) $(AVX512_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/kang_avx512i.o: src/kang_avx512i.c | $(BUILD)
	$(CC) $(COMMON) $(IFMA_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/kang_avx512i2.o: src/kang_avx512i2.c | $(BUILD)
	$(CC) $(COMMON) $(IFMA_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/bsgs_x86.o: src/bsgs_x86.c | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/bsgs_avx2v.o: src/bsgs_avx2v.c | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/bsgs_avx512v.o: src/bsgs_avx512v.c | $(BUILD)
	$(CC) $(COMMON) $(AVX512_FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/bsgs_avx512i.o: src/bsgs_avx512i.c | $(BUILD)
	$(CC) $(COMMON) $(IFMA_FLAGS) -MMD -MP -c $< -o $@

ifeq ($(X86),1)
ALLENG := $(ENGOBJ) $(KVOBJ) $(BSVOBJ)
TESTENG := $(ENGOBJ) $(BSVOBJ)       # the unit tests do not link the kangaroo
else
ALLENG := $(BUILD)/eng_base.o
ifeq ($(ARM64),1)
ALLENG += $(BUILD)/eng_neon.o $(BUILD)/eng_neon2.o
endif
TESTENG := $(ALLENG)
endif

$(BIN): $(OBJS) $(ALLENG)
	$(CC) $(COMMON) -o $@ $^ $(LDLIBS)

# Unit tests (built against the same sources)
TESTS := t_core t_perm t_jobq t_engine t_fe t_fe_asm t_puzzles t_eta t_bsgs t_fuzz t_ui t_gpufe
ifeq ($(X86),1)
TESTS += t_fev2 t_fev5 t_fev52
endif
TESTOBJS := $(filter-out $(BUILD)/main.o $(BUILD)/selftest.o $(BUILD)/run_common.o $(BUILD)/run_linear.o $(BUILD)/run_kangaroo.o $(BUILD)/bench_tg.o $(BUILD)/profile.o $(BUILD)/doctor.o $(BUILD)/gpu.o $(BUILD)/rt.o $(BUILD)/rt_cl.o $(BUILD)/rt_cuda.o $(BUILD)/cl_api.o $(BUILD)/kernels_gen.o $(BUILD)/kangaroo.o $(BUILD)/kang_base.o $(BUILD)/run_bsgs.o,$(OBJS))

$(BUILD)/t_fe_asm$(EXE): tests/t_fe.c $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -o $@ $< $(TESTOBJS) $(TESTENG) $(LDLIBS)

$(BUILD)/t_fev2$(EXE): tests/t_fev.c $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) $(AVX2_FLAGS) -DVBYTES=32 -o $@ $< $(TESTOBJS) $(TESTENG) $(LDLIBS)

$(BUILD)/t_fev5$(EXE): tests/t_fev.c $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) $(AVX512_FLAGS) -DVBYTES=64 -o $@ $< $(TESTOBJS) $(TESTENG) $(LDLIBS)

$(BUILD)/t_fev52$(EXE): tests/t_fev52.c $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) $(IFMA_FLAGS) -o $@ $< $(TESTOBJS) $(TESTENG) $(LDLIBS)

# t_gpufe: the kernel field arithmetic compiled for the host, PTX carry-chain variant (emulated) and portable variant
$(BUILD)/kfe_fast.o: tests/mock_cuda/kern_fe.c tests/mock_cuda/pz_host.h src/kernels/pz_common.cl | $(BUILD)
	$(CC) $(DEPLOY) -std=gnu11 -O2 -w -Itests/mock_cuda -DPZ_FAST_FE -DGK=gf_ -c $< -o $@
$(BUILD)/kfe_port.o: tests/mock_cuda/kern_fe.c tests/mock_cuda/pz_host.h src/kernels/pz_common.cl | $(BUILD)
	$(CC) $(DEPLOY) -std=gnu11 -O2 -w -Itests/mock_cuda -DGK=gp_ -Dpz_mock_gid=pz_mock_gid2 -c $< -o $@
$(BUILD)/t_gpufe$(EXE): tests/t_gpufe.c $(BUILD)/kfe_fast.o $(BUILD)/kfe_port.o $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) -o $@ $< $(BUILD)/kfe_fast.o $(BUILD)/kfe_port.o $(TESTOBJS) $(TESTENG) $(LDLIBS)

$(BUILD)/t_%$(EXE): tests/t_%.c $(TESTOBJS) $(TESTENG) | $(BUILD)
	$(CC) $(COMMON) -o $@ $< $(TESTOBJS) $(TESTENG) $(LDLIBS)

test: $(BIN) $(addsuffix $(EXE),$(addprefix $(BUILD)/,$(TESTS)))
	@for t in $(TESTS); do $(RUN) ./$(BUILD)/$$t$(EXE) || exit 1; done
	$(RUN) ./$(BIN) selftest

WINCC ?= x86_64-w64-mingw32-gcc-posix
windows:
	$(MAKE) CC=$(WINCC) BUILD=build/win all

wintest:
	$(MAKE) CC=$(WINCC) BUILD=build/win test

e2e: $(BIN)
	tests/e2e.sh $(BIN)

asan:
	$(MAKE) BUILD=build/asan OPT="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" COMMON_EXTRA= all

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

$(MOCK)/ptx_gen.c: $(PTX) $(BUILD)/embed
	@mkdir -p $(MOCK)
	$(BUILD)/embed $@ $(foreach p,$(PTXMODS),mock_ptx_$(p) src/kernels/ptx/$(p).ptx)

$(MOCKLIB): $(MOCKSRC) $(MOCK)/ptx_gen.c
	$(foreach h,128 256 512,$(MOCKCC) -c tests/mock_cuda/kern.c -DMOCK_LINEAR -DHALF=$(h) -Dpz_search=mock_search$(h) -o $(MOCK)/k$(h).o;)
	$(foreach n,16 32 64 128 256,$(MOCKCC) -c tests/mock_cuda/kern.c -DMOCK_KANG -DNK=$(n) -Dpz_kang_step=mock_kang$(n) -o $(MOCK)/kk$(n).o;)
	$(foreach n,16 32 64 128 256,$(MOCKCC) -c tests/mock_cuda/kern.c -DMOCK_KANG -DNK=$(n) -DMC=2 -Dpz_kang_step=mock_kang$(n)b -o $(MOCK)/kk$(n)b.o;)
	$(foreach n,64 128,$(MOCKCC) -c tests/mock_cuda/kern.c -DMOCK_KANG -DNK=$(n) -DMC=4 -Dpz_kang_step=mock_kang$(n)c -o $(MOCK)/kk$(n)c.o;)
	$(MOCKCC) -c tests/mock_cuda/kern.c -DMOCK_TEST -o $(MOCK)/kt.o
	$(MOCKCC) -c $(MOCK)/ptx_gen.c -o $(MOCK)/ptx_gen.o
	$(MOCKCC) $(MOCKLD) -o $@ tests/mock_cuda/mock_cuda.c $(MOCK)/k128.o $(MOCK)/k256.o $(MOCK)/k512.o $(MOCK)/kk16.o $(MOCK)/kk32.o $(MOCK)/kk64.o $(MOCK)/kk128.o $(MOCK)/kk256.o $(MOCK)/kk16b.o $(MOCK)/kk32b.o $(MOCK)/kk64b.o $(MOCK)/kk128b.o $(MOCK)/kk256b.o $(MOCK)/kk64c.o $(MOCK)/kk128c.o $(MOCK)/kt.o $(MOCK)/ptx_gen.o

mockcuda: $(MOCKLIB)

winmockcuda:
	$(MAKE) CC=$(WINCC) BUILD=build/win mockcuda

clean:
	rm -rf build

-include $(DEPS)
