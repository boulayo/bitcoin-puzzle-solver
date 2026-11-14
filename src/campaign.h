/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* campaign.h - campaign configuration (immutable after creation) and the
 * on-disk layout of a campaign directory. */
#ifndef PZ_CAMPAIGN_H
#define PZ_CAMPAIGN_H

#include "common.h"
#include <pzcore/ec.h>

#define PZ_FORMAT_VERSION 1
/* The version a campaign that encrypts the key it finds needs (campaign.conf: requires=X.Y.Z, written with encrypt_pubkey): a version
 * older than it does not know the line and would write the key in plain. Raise it when a line is added that older versions must not
 * ignore either. (1.21.0 and 1.21.1 know encrypt_pubkey but not requires: they still ignore the line.) */
#define PZ_REQUIRES_ENCRYPT "1.21.0"
/* The version a shared walk needs (jumpseed=): an older version would walk its own jump table with a seed it does not have (seed= is empty:
 * the start key is never written). */
#define PZ_REQUIRES_NET "1.24.0"
/* The version a campaign with a step needs (step=): an older version would search every key of the range instead of the steps. */
#define PZ_REQUIRES_STEP "1.24.0"

typedef enum { MODE_LINEAR = 0, MODE_KANGAROO = 1, MODE_BSGS = 2 } search_mode;

/* Several computers on one linear search (--part): the blocks of the search order are dealt out in turns to `total` shares and
 * each computer scans the blocks whose position in the order has a remainder, modulo `total`, among its shares. Shares are numbered
 * from 0 like --node. A faster computer takes more shares ("0-6/9" against "7,8/9"). */
#define PART_MAX_TOTAL 256
#define PART_TEXT 700            /* room for the canonical text of the worst case: every other share of 256 */
typedef struct {
    unsigned total;                      /* 0: no split (the whole search order) */
    unsigned count;                      /* shares of this computer, 1..total-1 */
    unsigned short res[PART_MAX_TOTAL];  /* ascending, each < total */
} part_spec;
/* "1/4", "0,1/4", "0-2,5/8". "" and "0/1" mean no split; so does a list of all the shares. */
bool part_parse(const char *text, part_spec *out, char *err, size_t errn);
/* Canonical text: ascending, runs written a-b; "" for no split. */
void part_format(const part_spec *p, char *out, size_t n);
/* Deals `total` shares to n computers in proportion to their speeds (largest remainder, every computer at least one share); counts[i]
 * gets the shares of computer i, taken in order: computer 0 the first counts[0] shares, and so on. False when n > total or a speed is 0. */
bool part_split(const u64 *speed, unsigned n, unsigned total, unsigned *counts);

typedef struct {
    char name[96];
    char dir[512];
    int puzzle;                  /* 0 when a custom range is used */
    pz_u256 range_start, range_end; /* inclusive; with a step, of the index i: 1 to the number of keys */
    u8 target_h160[20];
    char address[64];            /* may be empty if only a hash160 was given */
    bool has_pubkey;
    u8 pubkey33[33];
    pz_ge pubkey;
    search_mode mode;
    u8 seed[32];
    unsigned chunk_bits;
    u32 format_version;
    char created_with[32];       /* program version that created the campaign */
    double created_at;
    unsigned dp_bits;            /* kangaroo: distinguished point bits (0 = auto) */
    u64 bsgs_t;                  /* bsgs: baby steps in the table (fixed when the campaign is created) */
    char part[PART_TEXT];        /* linear: this computer's shares of the search order (canonical part_format text, "" = all) */
    bool has_encrypt;            /* the key is encrypted to encrypt_pubkey when it is found (hunt --encrypt-pubkey / --encrypt-password); public data */
    u8 encrypt_pubkey33[33];
    pz_ge encrypt_pubkey;
    bool net;                    /* a shared walk (--jump-seed, --start-key: docs/en/NETWORK.md 6): seed is the start key, in memory only */
    u8 net_jumpseed[32];         /* the public seed of the jump table */
    char net_keyid[17];          /* seed_to_id of the start key: a resume with another key is refused */
    bool has_step;               /* --step: the keys are step_base + i * step for the index i of the range (1 to their number); the
                                  * engines count, the campaign schedules and the reports measure in i (docs/en/USAGE.md, "Steps") */
    pz_u256 step, step_base;     /* step_base = START - step mod n, so that START, the first key, is i = 1 */
} campaign_cfg;

const char *mode_name(search_mode m);
/* --step: the keys START, START + STEP, ... up to END become the index range 1 to their number. False, with the reason, when the step is
 * 0, or not below n. */
bool cfg_set_step(campaign_cfg *c, const pz_u256 *start, const pz_u256 *end, const pz_u256 *step, const char **why);
/* The key of index i: i itself, or step_base + i * step mod n. */
void cfg_key_of(const campaign_cfg *c, const pz_u256 *i, pz_u256 *key);
/* The index of a key, the other way; false when it is not one of the keys of the range. */
bool cfg_index_of(const campaign_cfg *c, const pz_u256 *key, pz_u256 *i);
/* The point whose key the kangaroo and BSGS look for in the range: the public key, or (pubkey - step_base * G) / step, whose key is the
 * index. False when there is none (the public key is step_base * G, a key of no step). */
bool cfg_search_pubkey(const campaign_cfg *c, pz_ge *q);
/* The range as the reports show it: "0xSTART - 0xEND", or with a step "0xSTART - 0xLAST, step 0xSTEP" (cap: 3 * PZ_U256_HEX_BUF + 40). */
char *cfg_range_text(const campaign_cfg *c, char *out, size_t cap);
/* Range size = end - start + 1 (pz_u256). */
pz_u256 cfg_range_size(const campaign_cfg *c);
/* The default size of the blocks of the visiting order (bits) for a range; the stored value of an existing campaign wins. */
unsigned cfg_default_chunk_bits(const pz_u256 *range_size);   /* BSGS (the giant steps) and the other modes */
unsigned cfg_linear_chunk_bits(const pz_u256 *range_size, unsigned shares);   /* new linear campaigns: it depends on the shares of --part too */
/* Generates a fresh 256-bit seed with pzcore's generator (seed.h); dies only when the OS gives no random numbers. */
void seed_generate(u8 seed[32]);
/* Parses a user seed: 64 hex digits are used verbatim, anything else is hashed. */
void seed_from_text(const char *text, u8 seed[32]);
void seed_to_id(const u8 seed[32], char id[17]);   /* short fingerprint */

int cfg_save(const campaign_cfg *c, const char *path);
/* Refuses (with the message in err) a format it does not know, a requires=X.Y.Z line above this version, and invalid values. */
int cfg_load(campaign_cfg *c, const char *path, char *err, size_t errlen);
/* Compares two versions "X.Y.Z" part by part as numbers (10.0.0 is above 9.0.0; a suffix such as -dev is ignored): negative, 0 or
 * positive as a is below, equal to or above b; a text without three numbers counts as 0.0.0. */
int cfg_version_cmp(const char *a, const char *b);
/* Adds the line encrypt_pubkey=HEX to the campaign.conf at path, which has none (a campaign resumed with --encrypt-pubkey that was started without), keeping
 * every other line as it is. 0 on success. */
int cfg_add_encrypt_pubkey(const campaign_cfg *c, const char *path);

/* Puzzle n covers keys [2^(n-1), 2^n - 1]. */
bool puzzle_range(int n, pz_u256 *start, pz_u256 *end);

#endif
