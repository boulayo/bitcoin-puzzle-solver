# SPDX-License-Identifier: AGPL-3.0-or-later
# src/peer/peer.mk - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md), included by the Makefile.
# pzsolve itself does not change: it stays libc only. pzpeer links pzcore and the copy of libsecp256k1 in lib/secp256k1 (MIT, the library
# of Bitcoin Core, version in lib/secp256k1/VERSION), which gives it the constant-time operations on secret keys that pzcore does not
# promise: BIP340 signatures, ECDH and ECDSA.
#   make peer        build/pzpeer
#   make peer-test   the unit tests of src/peer (tests/peer/t_*.c)
#   make peer-fuzz   the mutation fuzzer of the readers of hostile input, FUZZ_SECONDS long (60)
#   make signer      build/pzsign, the two-party signer (signer/, Rust: needs cargo, and builds GMP from source the first time)
#   make signer-test its tests: two pzsign sessions relayed by the test (signer/tests/session.rs)

SECP_DIR   := lib/secp256k1
SECP_FLAGS  = -std=c99 $(OPT) -g $(DEPLOY) -I$(SECP_DIR) -I$(SECP_DIR)/include -w \
              -DENABLE_MODULE_ECDH=1 -DENABLE_MODULE_EXTRAKEYS=1 -DENABLE_MODULE_SCHNORRSIG=1
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
PEER_SRC   := json btc btc_rpc spv electrum netfile noise keys record net store dht dht_table dht_store dht_alert dht_relay bt upnp dpfeed pieces signer payout session chain_rpc chain_spv home main
PEER_OBJ   := $(addprefix $(BUILD)/peer/,$(addsuffix .o,$(PEER_SRC)))
PEER_LIBOBJ := $(filter-out $(BUILD)/peer/main.o,$(PEER_OBJ))
PEER_FLAGS  = $(LIBFLAGS) -Isrc/peer -I$(SECP_DIR)/include
PEER       := $(BUILD)/pzpeer$(EXE)
PEERTESTS  := t_json t_btc t_btc_rpc t_chain_rpc t_spv t_chain_spv t_noise t_record t_net t_store t_bt t_upnp t_dht t_pieces t_payout t_session t_trust
DEPS       += $(PEER_OBJ:.o=.d)

$(BUILD)/peer/%.o: src/peer/%.c | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -MMD -MP -c $< -o $@

$(PEER): $(PEER_OBJ) $(LIB) $(SECP_LIB)
	$(CC) $(LIBFLAGS) -o $@ $(PEER_OBJ) $(LIB) $(SECP_LIB) $(LIBLDLIBS)

$(BUILD)/peer/t_%$(EXE): tests/peer/t_%.c tests/peer/peertest.h $(PEER_LIBOBJ) $(LIB) $(SECP_LIB) | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -Itests/peer -o $@ $< $(PEER_LIBOBJ) $(LIB) $(SECP_LIB) $(LIBLDLIBS)
# the expected values of t_btc, written by tests/peer/gen_btc_vectors.py (python3 gen_btc_vectors.py > btc_vectors.h)
$(BUILD)/peer/t_btc$(EXE): tests/peer/btc_vectors.h

peer: $(PEER)

# The mutation fuzzer of the readers of hostile input (tests/peer/fuzz_peer.c), for FUZZ_SECONDS. It includes btc_rpc.c for the reader of
# the HTTP replies, so it links the other modules without btc_rpc.o. Best under the sanitizers (docs/en/NETWORK.md 8).
FUZZ_SECONDS ?= 60
FUZZ_OBJ    = $(filter-out $(BUILD)/peer/btc_rpc.o,$(PEER_LIBOBJ))
$(BUILD)/peer/fuzz_peer$(EXE): tests/peer/fuzz_peer.c src/peer/btc_rpc.c tests/peer/peertest.h tests/peer/btc_vectors.h $(FUZZ_OBJ) $(LIB) $(SECP_LIB) | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -Itests/peer -o $@ $< $(FUZZ_OBJ) $(LIB) $(SECP_LIB) $(LIBLDLIBS)

peer-fuzz: $(BUILD)/peer/fuzz_peer$(EXE)
	$(RUN) ./$(BUILD)/peer/fuzz_peer$(EXE) $(FUZZ_SECONDS)

# The tests of the two-party signing (t_pieces, t_session) run the real pzsign when it and two primes files are there: `make signer peer-primes`
# once (the primes take seconds to a minute), then every `make peer-test` uses them; without them those tests say they were skipped.
PEER_PRIMES := $(BUILD)/peer/primes0.json $(BUILD)/peer/primes1.json
peer-primes: | $(BUILD)/peer
	@for f in $(PEER_PRIMES); do [ -f $$f ] || ./$(BUILD)/pzsign$(EXE) primes $$f || exit 1; done

peer-test: $(PEER) $(addsuffix $(EXE),$(addprefix $(BUILD)/peer/,$(PEERTESTS)))
	@if [ -x $(BUILD)/pzsign$(EXE) ] && [ -f $(BUILD)/peer/primes0.json ] && [ -f $(BUILD)/peer/primes1.json ]; then \
	    export PZSIGN=$(BUILD)/pzsign$(EXE) PZSIGN_PRIMES0=$(BUILD)/peer/primes0.json PZSIGN_PRIMES1=$(BUILD)/peer/primes1.json; fi; \
	for t in $(PEERTESTS); do $(RUN) ./$(BUILD)/peer/$$t$(EXE) || exit 1; done
	$(RUN) ./$(PEER) version

# pzsign is a Rust crate with its own build: cargo decides what to rebuild, so the rule always runs it. --locked keeps the versions
# of signer/Cargo.lock. It is built for the machine that runs make (not with a MinGW cross compiler: GMP would need one too).
SIGNER      := $(BUILD)/pzsign$(EXE)
CARGO       ?= cargo
SIGNER_CARGO = $(CARGO) build --release --locked --manifest-path signer/Cargo.toml

signer:
	$(SIGNER_CARGO)
	@mkdir -p $(BUILD)
	cp signer/target/release/pzsign$(EXE) $(SIGNER)

signer-test:
	$(CARGO) test --release --locked --manifest-path signer/Cargo.toml

.PHONY: peer peer-test peer-fuzz peer-primes signer signer-test
