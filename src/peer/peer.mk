# SPDX-License-Identifier: AGPL-3.0-or-later
# src/peer/peer.mk - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md), included by the Makefile.
# pzsolve itself does not change: it stays libc only. pzpeer links pzcore and the copy of libsecp256k1 in lib/secp256k1 (MIT, the library
# of Bitcoin Core, version in lib/secp256k1/VERSION), which gives it the constant-time operations on secret keys that pzcore does not
# promise: BIP340 signatures, ECDH and ECDSA.
#   make peer        build/pzpeer
#   make peer-test   the unit tests of src/peer (tests/peer/t_*.c); make peer-tests only builds them
#   make peer-fuzz   the mutation fuzzer of the readers of hostile input, FUZZ_SECONDS long (60)
#   make signer      build/pzsign, the two-party signer (signer/, C++ on cb-mpc: needs a C++ compiler, and fetches and builds OpenSSL
#                    the first time)
#   make signer-test its tests: two pzsign sessions relayed by the test (signer/test.py)

SECP_DIR   := lib/secp256k1
SECP_FLAGS  = -std=c99 $(OPT) -g $(DEPLOY) -I$(SECP_DIR) -I$(SECP_DIR)/include -w \
              -DENABLE_MODULE_ECDH=1 -DENABLE_MODULE_EXTRAKEYS=1 -DENABLE_MODULE_SCHNORRSIG=1 -DSECP256K1_STATIC
SECP_OBJ   := $(addprefix $(BUILD)/secp256k1/,secp256k1.o precomputed_ecmult.o precomputed_ecmult_gen.o)
SECP_LIB   := $(BUILD)/libsecp256k1.a

$(BUILD)/secp256k1 $(BUILD)/peer:
	@mkdir -p $@

$(BUILD)/secp256k1/%.o: $(SECP_DIR)/src/%.c | $(BUILD)/secp256k1
	$(CC) $(SECP_FLAGS) -c $< -o $@

$(SECP_LIB): $(SECP_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# The modules of pzpeer, in the order of the layers (docs/en/NETWORK.md 2): main is the daemon and its commands.
# sys.h has one implementation per system (sys_posix.c, sys_win.c) and sys.c for what the two share
ifeq ($(WIN),1)
PEER_SYS   := sys sys_win
PEER_LIBS   = $(LIBLDLIBS) -lws2_32
else
PEER_SYS   := sys sys_posix
PEER_LIBS   = $(LIBLDLIBS)
endif
PEER_SRC   := $(PEER_SYS) dbg json btc btc_rpc spv electrum netfile noise keys record net store dht dht_table dht_store dht_alert dht_relay bt upnp dpfeed pieces signer payout session chain_rpc chain_spv sweep block home main
PEER_OBJ   := $(addprefix $(BUILD)/peer/,$(addsuffix .o,$(PEER_SRC)))
PEER_LIBOBJ := $(filter-out $(BUILD)/peer/main.o,$(PEER_OBJ))
PEER_FLAGS  = $(LIBFLAGS) -Isrc/peer -I$(SECP_DIR)/include -DSECP256K1_STATIC
PEER       := $(BUILD)/pzpeer$(EXE)
PEERTESTS  := t_sys t_json t_btc t_spv t_chain_spv t_noise t_record t_net t_store t_bt t_upnp t_dht t_pieces t_payout t_netfile t_block
# the tests whose stand-ins for bitcoind and pzsign are written for POSIX (sockets used directly, shell scripts): Linux and macOS
ifneq ($(WIN),1)
PEERTESTS  += t_btc_rpc t_chain_rpc t_session
endif
DEPS       += $(PEER_OBJ:.o=.d)

$(BUILD)/peer/%.o: src/peer/%.c | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -MMD -MP -c $< -o $@

$(PEER): $(PEER_OBJ) $(LIB) $(SECP_LIB)
	$(CC) $(LIBFLAGS) -o $@ $(PEER_OBJ) $(LIB) $(SECP_LIB) $(PEER_LIBS)

# PZ_TEST_DIR: where a test writes its files, its own build folder
$(BUILD)/peer/t_%$(EXE): tests/peer/t_%.c tests/peer/peertest.h $(PEER_LIBOBJ) $(LIB) $(SECP_LIB) | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -Itests/peer -DPZ_TEST_DIR=\"$(BUILD)/peer\" -o $@ $< $(PEER_LIBOBJ) $(LIB) $(SECP_LIB) $(PEER_LIBS)
# the expected values of t_btc, written by tests/peer/gen_btc_vectors.py (python3 gen_btc_vectors.py > btc_vectors.h)
$(BUILD)/peer/t_btc$(EXE): tests/peer/btc_vectors.h

peer: $(PEER)

# The mutation fuzzer of the readers of hostile input (tests/peer/fuzz_peer.c), for FUZZ_SECONDS. It includes btc_rpc.c for the reader of
# the HTTP replies, so it links the other modules without btc_rpc.o. Best under the sanitizers (docs/en/NETWORK.md 8).
FUZZ_SECONDS ?= 60
FUZZ_OBJ    = $(filter-out $(BUILD)/peer/btc_rpc.o,$(PEER_LIBOBJ))
$(BUILD)/peer/fuzz_peer$(EXE): tests/peer/fuzz_peer.c src/peer/btc_rpc.c tests/peer/peertest.h tests/peer/btc_vectors.h $(FUZZ_OBJ) $(LIB) $(SECP_LIB) | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -Itests/peer -o $@ $< $(FUZZ_OBJ) $(LIB) $(SECP_LIB) $(PEER_LIBS)

peer-fuzz: $(BUILD)/peer/fuzz_peer$(EXE)
	$(RUN) ./$(BUILD)/peer/fuzz_peer$(EXE) $(FUZZ_SECONDS)

# The tests of the two-party signing (t_pieces, t_session) run the real pzsign when it is there (`make signer` once); without it they say
# they were skipped.
peer-tests: $(PEER) $(addsuffix $(EXE),$(addprefix $(BUILD)/peer/,$(PEERTESTS)))

peer-test: peer-tests
	@if [ -x $(SIGNER) ]; then export PZSIGN=$(SIGNER); fi; \
	for t in $(PEERTESTS); do $(RUN) ./$(BUILD)/peer/$$t$(EXE) || exit 1; done
	$(RUN) ./$(PEER) version

# pzsign is C++ on the two-party ECDSA of cb-mpc (signer/vendor/cb-mpc, MIT, with the changes listed in signer/vendor/README.md), which
# needs a static OpenSSL 3 with one change of its own: signer/openssl.sh fetches it (checked by its hash) and builds it once per build
# folder, with $(CC). For the machine that runs make, or, with the MinGW-w64 compilers (make winsigner), for Windows. On macOS for the
# machine only (not with MACOS_ARCH).
SIGNER      := $(BUILD)/pzsign$(EXE)
CBMPC       := signer/vendor/cb-mpc
OSSL        := $(BUILD)/openssl
CBMPC_OBJ   := $(patsubst $(CBMPC)/src/%.cpp,$(BUILD)/cbmpc/%.o,$(wildcard $(CBMPC)/src/cbmpc/*/*.cpp))
SIGNER_CXXFLAGS = -std=c++17 -O2 -fno-operator-names -Wno-attributes -Wno-parentheses -DNDEBUG -DNO_DEPRECATED_OPENSSL -MMD -MP \
              $(if $(filter 1,$(X86)),-mpclmul -maes -msse4.1) $(if $(filter 1,$(ARM64)),-march=armv8-a+crypto) \
              -include $(CBMPC)/include/cbmpc/core/precompiled.h -I$(CBMPC)/vendors -I$(CBMPC)/include -I$(CBMPC)/include-internal -I$(OSSL)/include
ifeq ($(WIN),1)
SIGNER_LIBS := -static -lws2_32 -lcrypt32 -lbcrypt -luser32 -ladvapi32
else ifeq ($(DARWIN),1)
SIGNER_LIBS := -framework CoreServices -framework IOKit
else
SIGNER_LIBS := -static-libstdc++ -static-libgcc -lpthread -ldl
endif

$(OSSL)/lib/libcrypto.a: signer/openssl.sh
	CC="$(CC)" signer/openssl.sh $(abspath $(OSSL))

$(BUILD)/cbmpc/%.o: $(CBMPC)/src/%.cpp $(OSSL)/lib/libcrypto.a
	@mkdir -p $(@D)
	$(CXX) $(SIGNER_CXXFLAGS) -c $< -o $@

$(BUILD)/libcbmpc.a: $(CBMPC_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(SIGNER): signer/pzsign.cpp $(BUILD)/libcbmpc.a
	$(CXX) $(SIGNER_CXXFLAGS) -DPZ_VERSION=\"$(PZ_VERSION)\" -o $@ $< $(BUILD)/libcbmpc.a $(OSSL)/lib/libcrypto.a $(SIGNER_LIBS)

-include $(CBMPC_OBJ:.o=.d) $(SIGNER:$(EXE)=.d)

signer: $(SIGNER)

signer-test: $(SIGNER)
	$(RUN) ./$(SIGNER) selftest
	python3 -I signer/test.py $(if $(RUN),$(RUN) )./$(SIGNER)

.PHONY: peer peer-tests peer-test peer-fuzz signer signer-test
