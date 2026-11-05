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
    pz_u256 range_start, range_end; /* inclusive */
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
} campaign_cfg;

const char *mode_name(search_mode m);
/* Range size = end - start + 1 (pz_u256). */
pz_u256 cfg_range_size(const campaign_cfg *c);
/* The default size of the blocks of the visiting order (bits) for a range; the stored value of an existing campaign wins. */
unsigned cfg_default_chunk_bits(const pz_u256 *range_size);   /* BSGS (the giant steps) and the other modes */
unsigned cfg_linear_chunk_bits(const pz_u256 *range_size, unsigned shares);   /* new linear campaigns: it depends on the shares of --part too */
/* Generates a fresh 256-bit seed from OS entropy mixed with time/pid/host. */
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
