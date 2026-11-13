/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* store.h - the custodian (docs/en/NETWORK.md 7.3): the records a node keeps for the network, and the alert pairs it keeps as a mailbox.
 *
 * Records live in an append-only file, a 40-byte header ("PZSTORE1" and the fingerprint F of the network) and the 138-byte records one
 * after the other; in memory there is only an index by the first 8 bytes of K = SHA256("pznet1/key" || x) and the last sequence number of
 * each owner. The index is rebuilt from the file when it is opened, and a record cut by a crash at the end of the file is dropped.
 * A store refuses a second record with the same x from the same owner, and a new record whose sequence number is not above the last one
 * of its owner. Records with the same x from different owners are a coincidence: store_put gives them to the caller, which alerts.
 * A custodian's store has limits (store_limits): records in all, records of one owner, owners of one x; beyond them it is full.
 *
 * The file is ours, so the records are not checked again when it is opened, except the last STORE_CHECK_TAIL: a crash can leave a record
 * that is whole in length but not in content. A record that fails its check there is not indexed. The records store_put reports as a
 * coincidence are checked again when they are read back, so a damaged one never becomes an alert.
 *
 * Every function is thread-safe. */
#ifndef PZPEER_STORE_H
#define PZPEER_STORE_H

#include "record.h"
#include <stddef.h>

#define STORE_HDR 40
#define STORE_CHECK_TAIL 1024      /* records at the end of the file checked again when it is opened */
#define STORE_X_OWNERS 8           /* owners of one x a store keeps: more are no walk of the network, but someone's flood */

typedef enum {
    STORE_OK,                      /* stored */
    STORE_DUP,                     /* this owner's record with this x is already there (kept as it was) */
    STORE_INVALID,                 /* record_check refuses it */
    STORE_OLD,                     /* a new record whose sequence number is not above the last one of its owner */
    STORE_FAIL,                    /* the file could not be written */
    STORE_FULL                     /* over a limit: records in all, records of its owner, owners of its x */
} store_status;

typedef struct store store;

/* Opens (or creates) the store of a network at path. NULL, with the reason in err, for a file of another network or that cannot be read. */
store *store_open(const char *path, const secp256k1_context *ctx, const record_net *net, char *err, size_t errlen);
void store_close(store *st);
/* The limits of a custodian (0: none): records in all, records of one owner. Owners of one x are at most STORE_X_OWNERS in any store. */
void store_limits(store *st, uint64_t max_records, uint64_t max_per_owner);
/* Checks and stores r. mine: the record of r's owner with r's x that the store now holds (r itself, or the earlier one for STORE_DUP;
 * not set for STORE_FULL). others: the records with the same x from other owners, at most max, their number in *nothers (0 unless
 * STORE_OK, STORE_DUP or STORE_FULL): with any, a coincidence. mine and others may be NULL when max is 0. */
store_status store_put(store *st, const record *r, record *mine, record *others, int max, int *nothers);
/* The records with this x, at most max; their number. */
int store_find(store *st, const uint8_t x[32], record *out, int max);
uint64_t store_count(store *st);
/* The i-th record of the file (in the order they came); false past the end or for a damaged one. */
bool store_read(store *st, uint64_t i, record *out);
/* The last sequence number of an owner; false for an owner of whom the store has nothing. */
bool store_last_seq(store *st, const uint8_t owner[32], uint64_t *seq);
/* K = SHA256("pznet1/key" || x), the key of a record in the table. */
void store_key(const uint8_t x[32], uint8_t k[32]);

/* A coincidence: two valid records with the same x from different owners. */
bool store_pair_ok(const secp256k1_context *ctx, const record_net *net, const record *a, const record *b);

/* The mailbox: alert pairs kept for their owners, in memory, at most cap in all and per_owner of one owner (of the owner that has
 * too many, else of all, the oldest pair that no owner proved goes first, then the oldest). Two owners at one point with the same types
 * are one pair, whatever the sequence numbers of their records. A pair is kept in the order of its owners (the smaller x-only identity first), whatever order
 * it came in, with the proof of possession of each owner when one came (docs/en/NETWORK.md 7.3: over the pair, for the other owner). The
 * mailbox keeps what it is given: the caller checks pairs and proofs. */
typedef struct mbox mbox;

typedef struct {
    record r[2];
    bool has[2];                   /* the proof of the owner of r[i] came */
    uint8_t proof[2][64];
} mbox_pair;

mbox *mbox_new(size_t cap, size_t per_owner);
void mbox_free(mbox *m);
/* Keeps the pair (a, b) and, when proof is not NULL, the proof of the owner of a. False when nothing new came (the pair, and that proof,
 * were there). */
bool mbox_put(mbox *m, const record *a, const record *b, const uint8_t proof[64]);
/* The pairs with a record of owner, the newest first, at most max; their number. */
int mbox_get(mbox *m, const uint8_t owner[32], mbox_pair *out, int max);

#endif
