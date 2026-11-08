# SPDX-License-Identifier: AGPL-3.0-or-later
# src/peer/peer.mk - pzpeer, the daemon of the shared kangaroo network (docs/en/NETWORK.md), included by the Makefile.
# pzsolve itself does not change: it stays libc only. pzpeer links pzcore and the copy of libsecp256k1 in lib/secp256k1 (MIT, the library
# of Bitcoin Core, version in lib/secp256k1/VERSION), which gives it the constant-time operations on secret keys that pzcore does not
# promise: BIP340 signatures, ECDH and ECDSA.
#   make peer        build/pzpeer
#   make peer-test   the unit tests of src/peer (tests/peer/t_*.c)

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
PEER_SRC   := noise keys record dpfeed main
PEER_OBJ   := $(addprefix $(BUILD)/peer/,$(addsuffix .o,$(PEER_SRC)))
PEER_LIBOBJ := $(filter-out $(BUILD)/peer/main.o,$(PEER_OBJ)) $(BUILD)/netfile.o   # netfile.o: the reader of pzsolve, libc and pzcore only
PEER_FLAGS  = $(LIBFLAGS) -Isrc/peer -I$(SECP_DIR)/include
PEER       := $(BUILD)/pzpeer$(EXE)
PEERTESTS  := t_noise t_record
DEPS       += $(PEER_OBJ:.o=.d)

$(BUILD)/peer/%.o: src/peer/%.c | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -MMD -MP -c $< -o $@

$(PEER): $(PEER_OBJ) $(BUILD)/netfile.o $(LIB) $(SECP_LIB)
	$(CC) $(LIBFLAGS) -o $@ $(PEER_OBJ) $(BUILD)/netfile.o $(LIB) $(SECP_LIB) $(LIBLDLIBS)

$(BUILD)/peer/t_%$(EXE): tests/peer/t_%.c $(PEER_LIBOBJ) $(LIB) $(SECP_LIB) | $(BUILD)/peer
	$(CC) $(PEER_FLAGS) -Itests/peer -o $@ $< $(PEER_LIBOBJ) $(LIB) $(SECP_LIB) $(LIBLDLIBS)

peer: $(PEER)

peer-test: $(PEER) $(addsuffix $(EXE),$(addprefix $(BUILD)/peer/,$(PEERTESTS)))
	@for t in $(PEERTESTS); do $(RUN) ./$(BUILD)/peer/$$t$(EXE) || exit 1; done
	$(RUN) ./$(PEER) version

.PHONY: peer peer-test
