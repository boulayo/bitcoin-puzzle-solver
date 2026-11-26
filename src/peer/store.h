/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* store.h - the custodian (docs/en/NETWORK.md 7.3): the records a node keeps for the network, and the alert pairs it keeps as a mailbox.
 *
 * Records live in an append-only file, a 40-byte header ("PZSTORE2" and the fingerprint F of the network) and 38-byte entries one after
 * the other: the type, the zero bits, x and the number of the owner, whose identity is the entry of that number in a second file beside it
 * (PATH.owners: "PZOWNER2", F, then 32 bytes a owner, in the order they came). In memory there is only an index by a keyed hash of x
 * and the owners with their records held; both are rebuilt from the files when they are opened, and an entry cut by a crash at the end
 * of a file is dropped. A store refuses a second record with the same x from the same owner. Records with the same x
 * from different owners are a coincidence: store_put gives them to the caller. A custodian's store has limits (store_limit): records
 * in all, owners in all (one per STORE_OWNER_RECORDS records it may hold, and 1024 more), owners of one x (STORE_X_OWNERS, and as many
 * more whose owners proved them: store_put_proved); beyond them it is full.
 *
 * The files are ours, so the records are not checked again when they are opened, except the last STORE_CHECK_TAIL: a crash can leave an
 * entry that is whole in length but not in content. One that fails its check there is not indexed. The records store_put reports as a
 * coincidence are checked again when they are read back, so a damaged one never becomes an alert.
 *
 * Every function is thread-safe. */
#ifndef PZPEER_STORE_H
#define PZPEER_STORE_H

#include "record.h"
#include <stddef.h>

#define STORE_HDR 40
#define STORE_ENTRY 38             /* an entry of the file: type, zero bits, x, the number of the owner */
#define STORE_CHECK_TAIL 1024      /* records at the end of the file checked again when it is opened */
#define STORE_OWNER_RECORDS 256    /* records a store may hold for each owner it may hold: an identity costs little, and owners take memory
                                    * and disk for ever, so a custodian counts a new one as this many records (dht_store.c) */
#define STORE_X_OWNERS 8           /* owners of one x a store keeps: more are no walk of the network, but someone's flood (or copies
                                    * planted to fill the x before its owner's record: that one comes in with its proof) */

typedef enum {
    STORE_OK,                      /* stored */
    STORE_DUP,                     /* this owner's record with this x is already there (kept as it was) */
    STORE_INVALID,                 /* record_check refuses it */
    STORE_FAIL,                    /* the file could not be written */
    STORE_FULL                     /* over a limit: records in all, records of its owner, owners of its x */
} store_status;

typedef struct store store;

/* A file of entries of unit bytes after a STORE_HDR-byte header (magic, the fingerprint of the network), opened or made: the number of
 * entries, one cut by a crash at the end dropped; -1 with the reason in *why. */
int64_t store_file(const char *path, const char magic[8], const uint8_t fp[32], size_t unit, int *fd, const char **why);

/* Opens (or creates) the store of a network at path (and PATH.owners). NULL, with the reason in err, for a file of another network or that
 * cannot be read. */
store *store_open(const char *path, const secp256k1_context *ctx, const record_net *net, char *err, size_t errlen);
void store_close(store *st);
/* The limit of a custodian (0: none): records in all, and with it owners in all. Owners of one x are at most STORE_X_OWNERS in any store
 * (with those proved, twice as many). */
void store_limit(store *st, uint64_t max_records);
/* Whether the store knows this owner: it took a record of it (an owner once taken stays). */
bool store_owner(store *st, const uint8_t owner[32]);
/* Checks and stores r. mine: the record of r's owner with r's x that the store now holds (r itself, or the earlier one for STORE_DUP;
 * not set for STORE_FULL). others: the records with the same x from other owners, the newest first, at most max, their number in
 * *nothers (0 unless STORE_OK, STORE_DUP or STORE_FULL): with any, a coincidence. mine and others may be NULL when max is 0. */
store_status store_put(store *st, const record *r, record *mine, record *others, int max, int *nothers);
/* store_put of a record whose owner proved it (its proof of possession checked out): taken also when its x has STORE_X_OWNERS owners,
 * up to twice as many. Copies planted before the real record cannot keep it out, and the newest first shows it to the next owner.
 * others, max and nothers as for store_put. */
store_status store_put_proved(store *st, const record *r, record *others, int max, int *nothers);
/* The records with this x, the newest first, at most max (and 2 * STORE_X_OWNERS); their number. */
int store_find(store *st, const uint8_t x[32], record *out, int max);
uint64_t store_count(store *st);
/* The i-th record of the file (in the order they came); false past the end or for a damaged one. */
bool store_read(store *st, uint64_t i, record *out);
/* The jumps the records stand for: the sum of 2^dp over the records, each the work of a walk that finds one point in 2^dp. */
long double store_weight(store *st);
/* K = SHA256("pznet1/key" || x), the key of a record in the table. */
void store_key(const uint8_t x[32], uint8_t k[32]);

/* A coincidence: two valid records with the same x from different owners. */
bool store_pair_ok(const secp256k1_context *ctx, const record_net *net, const record *a, const record *b);
/* The records of the pair (a, b) in the order of their owners, the smaller x-only identity first, as a pair is named, kept and carried
 * (MBOX): out[0] and out[1]. The place of a in it. */
int pair_order(const record *a, const record *b, record out[2]);
/* SHA256("pznet1/pair" || the names of the two records in that order): the name of a pair, whichever order it came in; also the nonce
 * of the proof an owner sends for a pair (docs/en/NETWORK.md 7.3). */
void pair_hash(const record *a, const record *b, uint8_t h[32]);

/* The mailbox: alert pairs kept for their owners, in memory, at most cap in all and per_owner of one owner (of the owner that has
 * too many, else of all, the oldest pair goes). Two owners at one point with the same types are one pair, whatever the zero bits of
 * their records. A pair is kept in the order of its owners (pair_order), whatever order it came in, with the proof of possession of each
 * owner that sent one (docs/en/NETWORK.md 7.3: over the pair, for the other owner). The mailbox keeps what it is given: the caller checks
 * pairs and proofs. */
typedef struct mbox mbox;

typedef struct {
    record r[2];
    bool has[2];                   /* the proof of the owner of r[i] came */
    uint8_t proof[2][64];
} mbox_pair;

mbox *mbox_new(size_t cap, size_t per_owner);
void mbox_free(mbox *m);
/* Keeps the pair (a, b) with the proof of the owner of a. False when nothing new came (the pair, with that owner's proof, was there). */
bool mbox_put(mbox *m, const record *a, const record *b, const uint8_t proof[64]);
/* The pairs with a record of owner, the newest first, at most max; their number. */
int mbox_get(mbox *m, const uint8_t owner[32], mbox_pair *out, int max);

#endif
