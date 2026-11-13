/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_hunt.c - 'pzsolve hunt': the campaign of the options (a new one, or the one that exists with the options checked against it),
 * the session (the settings, the log header, the run, the end with its reports) and the key when it is found; the encryption of the key
 * and the banner are shared with 'merge' (cmd.h). */
#include "cmd.h"
#include "gpu.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include "profile.h"
#include "kangaroo.h"
#include "puzzles.h"
#include "campaigns.h"
#include "bsgs.h"
#include "platform.h"
#include "envopts.h"
#include "encrypt.h"
#include "onfound.h"
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* PZ_ISA: true when it imposes a CPU engine ("auto" and unset do not). Checked before a campaign directory is created, so a typo leaves
 * nothing behind. */
static bool isa_forced(void)
{
    const char *isa = g_env.isa;
    if (!isa[0] || !strcmp(isa, "auto")) return false;
    const cpu_engine *e = cpu_engine_by_name(isa);
    if (!e) pz_die("PZ_ISA: invalid value '%s' (expected %s)", isa, PZ_ISA_LIST);
    if (!e->supported()) pz_die("PZ_ISA=%s: the CPU does not support this instruction set (see 'pzsolve devices')", isa);
    return true;
}

static bool parse_pubkey(const char *hex, campaign_cfg *c)
{
    u8 b[65];
    int n = pz_hex_decode(hex, b, 65);
    if (n != 33 && n != 65) return false;
    pz_ge p;
    if (!pz_ge_from_pubkey(&p, b, (size_t)n)) return false;
    c->pubkey = p;
    pz_ge_to_pub33(c->pubkey33, &p);
    c->has_pubkey = true;
    return true;
}

/* The baby steps the options ask for: the table of an existing --table file (it decides, whatever --bsgs-entries says), otherwise
 * --bsgs-entries; 0 when they ask for none. *from_table tells which of the two it is. */
static u64 requested_bsgs_entries(const opts_t *o, bool *from_table)
{
    u64 T;
    char err[256];
    *from_table = o->table[0] && access(o->table, F_OK) == 0;
    if (!*from_table) return o->bsgs_entries;
    if (!bsgs_table_peek(o->table, &T, err, sizeof err)) pz_die("table %s is not usable: %s", o->table, err);
    return T;
}

/* Table size for a bsgs campaign: taken from an existing --table file, forced with --bsgs-entries, or the best one that
 * fits the memory budget (70% of the available RAM, or --mem). *viable (optional) tells whether it beats the kangaroo. */
static u64 pick_bsgs_entries(const opts_t *o, const pz_u256 *rsz, bool *viable)
{
    bool from_table;
    u64 T = requested_bsgs_entries(o, &from_table);
    if (T) {
        if (viable) *viable = bsgs_mean_cost(rsz, T) <= kang_mean_cost(rsz);
        return T;
    }
    sysinfo_t si;
    sysinfo_detect(&si);
    u64 budget = o->mem_gb > 0 ? mem_bytes(o->mem_gb) : (u64)(0.70 * (double)si.mem_avail);
    T = bsgs_choose_T(rsz, budget, viable);
    if (T < 1) pz_die("not enough memory for a BSGS table");
    return T;
}

/* The text of --range, "START:END" in hexadecimal. On failure *why is the message to give. */
static bool range_parse(const char *text, pz_u256 *a, pz_u256 *b, const char **why)
{
    char range_copy[200];
    pz_strlcpy(range_copy, text, sizeof range_copy);
    char *colon = strchr(range_copy, ':');
    if (!colon) { *why = "--range requires the format START:END (hexadecimal)"; return false; }
    *colon = 0;
    if (!pz_u256_from_hex(a, range_copy) || !pz_u256_from_hex(b, colon + 1)) { *why = "--range: invalid hexadecimal values"; return false; }
    return true;
}

/* Name used when -c is absent. It depends only on what identifies the search (puzzle, range, target), so repeating the same
 * command resumes the campaign instead of starting a new one. */
static void derive_campaign_name(const opts_t *o, char *out, size_t n)
{
    if (o->puzzle && !o->range[0]) {
        snprintf(out, n, "p%d", o->puzzle);
        return;
    }
    char rng[300] = "", key[900];
    if (o->range[0]) {
        pz_u256 a, b;
        const char *why;
        if (range_parse(o->range, &a, &b, &why)) {
            char ha[80], hb[80];
            pz_u256_to_hex(&a, ha);
            pz_u256_to_hex(&b, hb);
            snprintf(rng, sizeof rng, "%s:%s", ha, hb);   /* normalised: leading zeros / case do not matter */
        } else pz_strlcpy(rng, o->range, sizeof rng);
    }
    /* the empty field is where a hash160 target went until 1.22.0: kept, so that the names of the campaigns made since then stay the same */
    int kl = snprintf(key, sizeof key, "PZNAME1|%d|%s|%s||%s", o->puzzle, rng, o->address, o->pubkey);
    u8 h[32];
    char id[17];
    pz_sha256((const u8 *)key, (size_t)(kl < (int)sizeof key ? kl : (int)sizeof key - 1), h);
    pz_hex_encode(h, 4, id);
    if (o->puzzle) snprintf(out, n, "p%d-%s", o->puzzle, id);
    else snprintf(out, n, "custom-%s", id);
}

/* The block size (bits) of a campaign over rsz keys: the one asked for with --chunk-bits or the default of the range, and never more than
 * the range holds. A new campaign takes this value and a resumed command is compared through it, so that the command that created a
 * campaign can resume it also when its --chunk-bits was cut down. */
static unsigned clamp_chunk_bits(const pz_u256 *rsz, unsigned cb)
{
    pz_u256 chunk = pz_u256_pow2(cb);
    if (pz_u256_cmp(&chunk, rsz) > 0) {   /* chunk larger than the range: shrink */
        unsigned b = pz_u256_bits(rsz) - 1;
        cb = b < 1 ? 1 : b;
    }
    return cb;
}

static unsigned effective_chunk_bits(const pz_u256 *rsz, int requested)
{
    return clamp_chunk_bits(rsz, requested > 0 ? (unsigned)requested : cfg_default_chunk_bits(rsz));
}

/* -m kangaroo or -m bsgs without a public key: what can be done instead. */
static void no_pubkey(const campaign_cfg *c, const char *mode)
{
    if (c->puzzle && puzzle_find(c->puzzle) && !puzzle_find(c->puzzle)->pubkey[0])
        pz_die("puzzle %d has no known public key: only -m linear applies", c->puzzle);
    pz_die("%s mode needs the public key: give it with -k, or use -m linear", mode);
}

/* The warnings about a solved puzzle, once the command has been checked whole: a command that is refused gets only its error. */
static void warn_solved(const opts_t *o, const campaign_cfg *c)
{
    const puzzle_info *db = o->puzzle ? puzzle_find(o->puzzle) : NULL;
    if (!db || !db->privkey[0] || o->address[0] || o->pubkey[0]) return;   /* only for the target of the database */
    pz_log(PZ_LOG_WARN, "puzzle %d is already SOLVED (the key is public): the campaign is only a test run", db->n);
    pz_u256 pk;
    if (o->range[0] && pz_u256_from_hex(&pk, db->privkey) && (pz_u256_cmp(&pk, &c->range_start) < 0 || pz_u256_cmp(&pk, &c->range_end) > 0))
        pz_log(PZ_LOG_WARN, "the known key of puzzle %d is not in the given range: the campaign will not find it", db->n);
}

/* The target of a new campaign (c->target_h160, c->address, the public key): from the options, or from the database for -p N. */
static void set_target(const opts_t *o, campaign_cfg *c)
{
    bool have_target = false;
    const puzzle_info *db = o->puzzle ? puzzle_find(o->puzzle) : NULL;
    bool from_db = false;
    if (db && !o->address[0] && !o->pubkey[0]) {
        /* target taken from the built-in database */
        u8 h[20];
        if (!pz_address_to_hash160(db->address, h)) pz_die("puzzle database corrupted (puzzle %d)", db->n);
        memcpy(c->target_h160, h, 20);
        pz_strlcpy(c->address, db->address, sizeof c->address);
        have_target = true;
        from_db = true;
        if (db->pubkey[0] && parse_pubkey(db->pubkey, c)) {
            u8 hp[20];
            pz_hash160(c->pubkey33, 33, hp);
            if (memcmp(hp, h, 20) != 0) pz_die("puzzle database inconsistent (puzzle %d)", db->n);
        }
        if (db->privkey[0]) {
            /* a solved puzzle: the stored key must map to the stored address, otherwise the database is damaged */
            pz_u256 pk;
            pz_ge pt;
            u8 pub[33], hk[20];
            if (!pz_u256_from_hex(&pk, db->privkey) || !pz_ec_mul_g(&pt, &pk)) pz_die("puzzle database corrupted (key of puzzle %d)", db->n);
            pz_ge_to_pub33(pub, &pt);
            pz_hash160(pub, 33, hk);
            if (memcmp(hk, h, 20) != 0) pz_die("puzzle database inconsistent (the key of puzzle %d does not match the address)", db->n);
        }
        if (o->range[0]) {
            /* a custom range must overlap the range the puzzle guarantees */
            pz_u256 pa, pb;
            if (puzzle_range(o->puzzle, &pa, &pb)) {
                if (pz_u256_cmp(&c->range_end, &pa) < 0 || pz_u256_cmp(&c->range_start, &pb) > 0)
                    pz_die("the given range does not intersect the one of puzzle %d: the key cannot be there", o->puzzle);
                if (pz_u256_cmp(&c->range_start, &pa) < 0 || pz_u256_cmp(&c->range_end, &pb) > 0)
                    pz_log(PZ_LOG_WARN, "the given range extends beyond the one of puzzle %d: the outside part cannot contain the key", o->puzzle);
            }
        }
    }
    if (o->pubkey[0]) {
        if (!parse_pubkey(o->pubkey, c)) pz_die("invalid public key (expected 33 or 65 bytes in hex, a point on the curve)");
        pz_hash160(c->pubkey33, 33, c->target_h160);
        have_target = true;
    }
    if (o->address[0]) {
        u8 h[20];
        if (!pz_address_to_hash160(o->address, h)) pz_die("invalid address (a mainnet \"1...\" address or a native SegWit \"bc1q...\" address with a valid checksum is expected)");
        if (have_target && memcmp(h, c->target_h160, 20) != 0)
            pz_die("the given public key does not match the address (the hash160 of the compressed form differs)");
        memcpy(c->target_h160, h, 20);
        pz_strlcpy(c->address, o->address, sizeof c->address);
        have_target = true;
    }
    if (db && !from_db) {
        u8 hd[20];
        if (pz_address_to_hash160(db->address, hd) && memcmp(hd, c->target_h160, 20) != 0)
            pz_log(PZ_LOG_WARN, "the given target is not the one of puzzle %d in the database (%s): using the given one", db->n, db->address);
    }
    const char *retired = o->puzzle ? puzzle_retired_address(o->puzzle) : NULL;
    if (!have_target && retired) {
        /* puzzles 161..256: the address is known, the funds are gone */
        u8 h[20];
        if (!pz_address_to_hash160(retired, h)) pz_die("puzzle database corrupted (puzzle %d)", o->puzzle);
        memcpy(c->target_h160, h, 20);
        pz_strlcpy(c->address, retired, sizeof c->address);
        have_target = true;
        pz_log(PZ_LOG_WARN, "puzzle %d was emptied by the creator in 2017 (block 475240): address %s holds no "
               "funds, the search is only an exercise (and with 2^%d keys it cannot finish)", o->puzzle, retired, o->puzzle - 1);
    }
    if (!have_target) pz_die("a target is required: --address or --pubkey");
    if (!c->address[0]) pz_hash160_to_address(c->target_h160, c->address, sizeof c->address);
}

/* The method of a new campaign (-m); auto picks BSGS when its table fits and it is the better choice, the kangaroo otherwise, the linear
 * search without a public key or for a small range. */
static void set_mode(const opts_t *o, campaign_cfg *c, const pz_u256 *rsz)
{
    if (!strcmp(o->mode, "linear")) c->mode = MODE_LINEAR;
    else if (!strcmp(o->mode, "kangaroo")) {
        if (!c->has_pubkey) no_pubkey(c, "kangaroo");
        c->mode = MODE_KANGAROO;
    } else if (!strcmp(o->mode, "bsgs")) {
        if (!c->has_pubkey) no_pubkey(c, "bsgs");
        c->mode = MODE_BSGS;
        c->bsgs_t = pick_bsgs_entries(o, rsz, NULL);
    } else if (!strcmp(o->mode, "auto")) {
        pz_u256 lim = pz_u256_pow2(20);
        if (!c->has_pubkey || pz_u256_cmp(rsz, &lim) < 0) c->mode = MODE_LINEAR;
        else {
            bool viable;
            u64 T = pick_bsgs_entries(o, rsz, &viable);
            bool given = o->table[0] && access(o->table, F_OK) == 0;
            bool small = viable && bsgs_mean_cost(rsz, T) < 4e9L;               /* seconds of work: no point in waking the GPUs */
            bool gpus = !o->no_gpu && gpu_count_usable() > 0;
            if (viable && (given || small || !gpus || o->bsgs_entries)) {
                c->mode = MODE_BSGS;
                c->bsgs_t = T;
            } else c->mode = MODE_KANGAROO;
        }
    } else pz_die("--mode: invalid value '%s' (expected auto, linear, kangaroo or bsgs)", o->mode);
}

/* Builds a new campaign configuration from the command line. */
static void build_new_cfg(const opts_t *o, campaign_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->format_version = PZ_FORMAT_VERSION;
    pz_strlcpy(c->created_with, PZ_VERSION, sizeof c->created_with);
    c->created_at = pz_now();
    if (o->range[0]) {
        const char *why;
        if (!range_parse(o->range, &c->range_start, &c->range_end, &why)) pz_die("%s", why);
        c->puzzle = o->puzzle;
    } else if (o->puzzle) {
        if (!puzzle_range(o->puzzle, &c->range_start, &c->range_end)) pz_die("puzzle %d is outside the supported limits (1..256)", o->puzzle);
        c->puzzle = o->puzzle;
    } else {
        pz_die("specify the range with --puzzle N or --range START:END");
    }
    if (pz_u256_is_zero(&c->range_start)) pz_die("the range must start from a key >= 1");
    if (pz_u256_cmp(&c->range_start, &c->range_end) > 0) pz_die("empty range (start > end)");
    if (pz_u256_cmp(&c->range_end, &pz_secp256k1_n) >= 0) pz_die("the range exceeds the order of the secp256k1 group");

    set_target(o, c);

    if (o->seed[0] && strcmp(o->seed, "auto") != 0) seed_from_text(o->seed, c->seed);
    else if (!o->jump_seed[0]) seed_generate(c->seed);   /* a shared walk takes the start key (shared_bind) */

    pz_u256 rsz = cfg_range_size(c);
    c->chunk_bits = effective_chunk_bits(&rsz, o->chunk_bits);
    c->dp_bits = o->dp_bits > 0 ? (unsigned)o->dp_bits : 0;

    set_mode(o, c, &rsz);
    if (c->mode == MODE_KANGAROO) {                  /* before anything is created on disk */
        const char *why = kang_range_error(&rsz);
        if (why) pz_die("%s", why);
    }

    if (o->part[0] && strcmp(o->part, "0/1") != 0) {
        if (c->mode != MODE_LINEAR) pz_die("--part is only used in linear search (with a known public key use -m linear; the kangaroo has --node and 'merge')");
        pz_strlcpy(c->part, o->part, sizeof c->part);
    }
    if (c->mode == MODE_LINEAR) {                 /* before anything is created on disk: the order of visit must exist and every share must hold a block */
        char perr[300];
        if (o->chunk_bits <= 0) {                 /* the default block of the linear search depends on the number of shares too (campaign.c) */
            part_spec ps;
            unsigned shares = c->part[0] && part_parse(c->part, &ps, perr, sizeof perr) && ps.total ? ps.total : 1;
            c->chunk_bits = clamp_chunk_bits(&rsz, cfg_linear_chunk_bits(&rsz, shares));
        }
        if (!sched_check_part(c, perr, sizeof perr)) pz_die("%s", perr);
    }

    pz_strlcpy(c->name, o->campaign, sizeof c->name);   /* always set by cmd_hunt (explicit or derived) */
}

/* Options that describe the campaign itself must agree with the stored configuration: a silent mismatch would make the
 * user believe a different search is running than the one that resumes. */
static void check_resume_consistency(const opts_t *o, const campaign_cfg *c)
{
    if (o->puzzle && c->puzzle != o->puzzle) {
        if (c->puzzle) pz_die("campaign '%s' is for puzzle %d, not %d", c->name, c->puzzle, o->puzzle);
        pz_die("campaign '%s' is not associated with a puzzle of the database (use -c %s without -p)", c->name, c->name);
    }
    if (o->range[0]) {
        pz_u256 a, b;
        const char *why;
        if (!range_parse(o->range, &a, &b, &why)) pz_die("%s", why);
        if (pz_u256_cmp(&a, &c->range_start) != 0 || pz_u256_cmp(&b, &c->range_end) != 0)
            pz_die("--range does not match the range of the existing campaign (the range cannot be changed; use another name with -c)");
    }
    if (o->address[0]) {
        u8 h[20];
        if (!pz_address_to_hash160(o->address, h) || memcmp(h, c->target_h160, 20) != 0) pz_die("the given address does not match the one of the existing campaign");
    }
    if (o->pubkey[0]) {
        campaign_cfg tmp;
        memset(&tmp, 0, sizeof tmp);
        if (!parse_pubkey(o->pubkey, &tmp)) pz_die("invalid public key (expected 33 or 65 bytes in hex, a point on the curve)");
        if (!c->has_pubkey || memcmp(tmp.pubkey33, c->pubkey33, 33) != 0) pz_die("the given public key does not match the one of the existing campaign");
    }
    check_encrypt_resume(o, c);
    if (o->seed[0] && strcmp(o->seed, "auto") != 0) {
        u8 s[32];
        seed_from_text(o->seed, s);
        if (memcmp(s, c->seed, 32) != 0) pz_die("the given seed does not match the one of the existing campaign (the seed cannot be changed)");
    }
    if (o->part[0]) {
        const char *want = strcmp(o->part, "0/1") ? o->part : "";
        if (strcmp(want, c->part) != 0)
            pz_die("--part %s does not match the split of the existing campaign (%s): the split cannot be changed, use another name with -c",
                   o->part, c->part[0] ? c->part : "none");
    }
    const pz_u256 rsz = cfg_range_size(c);
    if (o->chunk_bits > 0 && effective_chunk_bits(&rsz, o->chunk_bits) != c->chunk_bits)
        pz_die("--chunk-bits cannot be changed on an existing campaign (it is %u)", c->chunk_bits);
    if (strcmp(o->mode, "auto") != 0) {
        const char *want = !strcmp(o->mode, "linear") ? "linear" : !strcmp(o->mode, "kangaroo") ? "kangaroo" : !strcmp(o->mode, "bsgs") ? "bsgs" : NULL;
        if (!want) pz_die("--mode: invalid value '%s' (expected auto, linear, kangaroo or bsgs)", o->mode);
        if (strcmp(want, mode_name(c->mode)) != 0)
            pz_die("the existing campaign uses mode %s, not %s (the mode cannot be changed: use another name with -c)", mode_name(c->mode), want);
    }
    if (c->mode == MODE_BSGS) {
        bool from_table;
        const u64 T = requested_bsgs_entries(o, &from_table);
        if (T && T != c->bsgs_t) {
            char t1[32], t2[32];
            if (from_table) pz_die("table %s has %s baby steps but the campaign uses %s (this cannot be changed after creation)", o->table, pz_fmt_u64(T, t1, sizeof t1),
                                   pz_fmt_u64(c->bsgs_t, t2, sizeof t2));
            pz_die("--bsgs-entries cannot be changed on an existing campaign (its table has %s baby steps)", pz_fmt_u64(c->bsgs_t, t2, sizeof t2));
        }
    }
}

/* What the options must agree on with the campaign, tested as soon as its configuration is known. For a new campaign this happens before
 * its folder is created (fresh), so that a refused command leaves nothing behind; a campaign that exists is checked when it starts. */
static void check_options_for_campaign(const opts_t *o, const campaign_cfg *c, const run_ctx *r, bool fresh)
{
    if (o->node && c->mode != MODE_KANGAROO) pz_die("--node is only used with the kangaroo (in linear search split the range with -r)");
    if (fresh && c->mode == MODE_BSGS) run_bsgs_check(c, &r->sys, !o->no_cpu, mem_bytes(o->mem_gb), o->table, NULL);
}

/* The size of a range in words: "8,388,608 keys", "2^43 keys", "about 6.65e35 keys". */
static void size_text(const pz_u256 *sz, const char *dec, char *out, size_t n)
{
    const size_t len = strlen(dec);
    int bits = 0, top = -1;
    for (int i = 0; i < 256; i++)
        if ((sz->w[i / 64] >> (i % 64)) & 1) { bits++; top = i; }
    if (len <= 9 || (len <= 12 && bits != 1)) {
        char g[48];
        snprintf(out, n, "%s keys", pz_group_digits(dec, g, sizeof g));
    } else if (bits == 1) snprintf(out, n, "2^%d keys", top);
    else snprintf(out, n, "about %c.%c%ce%zu keys", dec[0], dec[1], dec[2], len - 1);
}

/* ---- encrypting the key that is found (docs/en/USAGE.md, "Encrypting the key") ---- */

/* Core dumps and same-user ptrace off, once: the plain key is in memory for a moment when it is found, and the password while it is typed. */
void harden_process(void)
{
    static bool done;
    char why[200];
    if (done) return;
    done = true;
    if (!pz_harden_process(why, sizeof why)) pz_log(PZ_LOG_WARN, "%s", why);
}

/* The public key to encrypt to, from --encrypt-pubkey or from the password typed (--encrypt-password), into the campaign c. */
static void encrypt_from_options(const opts_t *o, campaign_cfg *c)
{
    pz_ge q;
    u8 q33[33];
    char err[240];
    if (o->encrypt_pubkey[0]) {
        if (!enc_parse_pubkey(o->encrypt_pubkey, &q, q33, err, sizeof err)) pz_die("--encrypt-pubkey: %s (nothing was started)", err);
    } else if (o->encrypt_password) {
        harden_process();
        enc_password_to_pubkey(true, &q, q33);
        char id[9], hx[67];
        enc_key_id(&q, id);
        pz_hex_encode(q33, 33, hx);
        printf("Public key made from the password (key id %s):\n  %s\nCompare them with the output of 'pzsolve encrypt-keygen' on a computer you trust.\n", id, hx);
    } else return;
    if (!enc_check(&q, err, sizeof err)) pz_die("%s (nothing was started)", err);
    c->encrypt_pubkey = q;
    memcpy(c->encrypt_pubkey33, q33, 33);
    c->has_encrypt = true;
}

/* A campaign that has its public key stored cannot be switched to another one by an option. */
void check_encrypt_resume(const opts_t *o, const campaign_cfg *c)
{
    if (!o->encrypt_pubkey[0] || !c->has_encrypt) return;
    pz_ge q;
    u8 q33[33];
    char err[240], have[9], want[9];
    if (!enc_parse_pubkey(o->encrypt_pubkey, &q, q33, err, sizeof err)) pz_die("--encrypt-pubkey: %s", err);
    if (memcmp(q33, c->encrypt_pubkey33, 33) == 0) return;
    enc_key_id(&c->encrypt_pubkey, have);
    enc_key_id(&q, want);
    pz_die("campaign '%s' already encrypts its key to the public key with id %s, not to %s: a campaign cannot be switched to another key. "
           "Resume it without --encrypt-pubkey to keep the stored key, or start a new campaign with another name (-c) for the new key", c->name, have, want);
}

/* A campaign that is resumed or merged, with its lock taken: one that has no public key yet gets the one of the options (stored in campaign.conf: the key
 * was not found yet, so nothing has been shown in plain); one that has it keeps it and no password is asked. */
void encrypt_on_resume(const opts_t *o, run_ctx *r)
{
    if (r->cfg.has_encrypt) {
        if (o->encrypt_password) pz_log(PZ_LOG_INFO, "the campaign already has its public key to encrypt to: no password is asked");
        return;
    }
    if (!o->encrypt_pubkey[0] && !o->encrypt_password) return;
    encrypt_from_options(o, &r->cfg);
    char conf[700];
    snprintf(conf, sizeof conf, "%s/campaign.conf", r->cfg.dir);
    if (cfg_add_encrypt_pubkey(&r->cfg, conf) != 0) pz_die("cannot add the public key to %s: %s", conf, strerror(errno));
    pz_log(PZ_LOG_INFO, "the campaign now encrypts the key it finds (the public key is stored in campaign.conf)");
    /* the log of the sessions before may show the seed (the line of --part), which an encrypting campaign keeps out of its files */
    char sd[65], log[720];
    pz_hex_encode(r->cfg.seed, 32, sd);
    snprintf(log, sizeof log, "%s/log.txt", r->cfg.dir);
    char *text = pz_read_file(log, NULL);
    size_t w = 0;
    for (char *p = text, *e; p && *p; p = e) {                 /* the lines without the seed, moved up in place */
        e = p + strcspn(p, "\n") + (p[strcspn(p, "\n")] == '\n');
        const char c = *e;
        *e = 0;
        const bool has = strstr(p, sd) != NULL;
        *e = c;
        if (!has) memmove(text + w, p, (size_t)(e - p)), w += (size_t)(e - p);
    }
    if (text && w < strlen(text) && pz_write_file_atomic(log, text, w, 0600) != 0)
        pz_log(PZ_LOG_ERR, "cannot rewrite %s without the seed: delete that file", log);
    pz_secure_zero(sd, sizeof sd);
    free(text);
}

/* What a campaign that encrypts the key says at the start of a session. */
void announce_encrypt(const run_ctx *r)
{
    if (!r->cfg.has_encrypt) return;
    harden_process();
    char id[9];
    enc_key_id(&r->cfg.encrypt_pubkey, id);
    pz_log(PZ_LOG_INFO, "the key will be encrypted to the public key with id %s when found: the screen, FOUND.txt and the log will carry only the encrypted line", id);
}

/* Printed under the box of a found key: what the program is for, and how to support it. The addresses are those of README.md, which
 * stays the official place for them: the message points there, so that a copy of the program with other addresses can be told apart. */
static void print_found_thanks(void)
{
    printf("\n  Congratulations, you solved it!\n"
           "  pzsolve was created to give everyone a real chance at these prizes.\n"
           "  If it helped you win, please consider a donation to its author:\n"
           "    Bitcoin:            bc1qfw2y5xzvsy6w70rwqfq6n65tahk906678c7tvz\n"
           "    Lightning address:  trivialalarm90@cake.cash\n"
           "  Check them against the README of https://github.com/boulayo/bitcoin-puzzle-solver\n\n");
}

/* Printed under the box when the public key of the address was not known (a linear search): how not to lose the prize while spending it.
 * Until the first spend nobody has that public key, and with a key this short whoever sees it finds the private key in seconds. */
static void print_spend_warning(const run_ctx *r)
{
    if (r->cfg.has_pubkey) return;
    printf("\n  BEFORE YOU SPEND IT: do not use a wallet. A transaction that waits in the public mempool shows the public key of\n"
           "  this address, and programs watch for it: from the public key they find a key this short in seconds and replace your\n"
           "  transaction with theirs. Give the transaction straight to a mining pool that takes private transactions.\n"
           "  `pzpeer sweep` makes and signs it without sending it.\n");
}

/* The banner printed when the key is found (by 'hunt' or by 'merge'). */
/* what becomes of a key that could not be saved: the state stays as it was before the find (end_session) */
static const char *found_again(const run_ctx *r)
{
    return r->cfg.mode == MODE_KANGAROO ? "(the walk of a new session is another: this screen may be the only copy of the key)"
                                        : "(its place stays unsearched in the campaign: the same command finds it again)";
}

void print_found_banner(const run_ctx *r)
{
    char addr[80];
    if (r->cfg.address[0]) pz_strlcpy(addr, r->cfg.address, sizeof addr);
    else pz_hash160_to_address(r->cfg.target_h160, addr, sizeof addr);
    printf("\a\n======================= KEY FOUND =======================\n");
    if (r->cfg.puzzle) printf("  puzzle:         #%d\n", r->cfg.puzzle);
    if (r->cfg.has_encrypt) {                       /* only the ciphertext: the key is not on the screen, and neither is its public key */
        printf("  address:        %s\n", addr);
        if (r->found_seal_failed) {
            printf("  The key was found but could NOT be encrypted (the system gave no random numbers): it was written nowhere. Search again.\n");
        } else {
            char id[9];
            enc_key_id(&r->cfg.encrypt_pubkey, id);
            printf("  The key is encrypted to the public key with id %s: only the owner of the matching secret can read it.\n", id);
            if (r->found_write_failed) printf("  NOT SAVED:      %s could not be written (see the log): copy the encrypted key from this screen\n"
                                              "                  %s\n", r->path_found, found_again(r));
            else printf("  saved in:       %s\n", r->path_found);
            printf("  at home:        pzsolve decrypt-key FOUND.txt\n  encrypted key (safe to copy anywhere):\n%s\n", r->found_sealed);
        }
        printf("=========================================================\n");
        if (!r->found_seal_failed) print_spend_warning(r), print_found_thanks();
        return;
    }
    char hx[80], wif[80];
    u8 kb[32];
    pz_u256_to_hex(&r->found_key, hx);
    pz_u256_to_be(&r->found_key, kb);
    pz_privkey_to_wif(kb, true, wif, sizeof wif);
    printf("  address:        %s\n  key (hex):      0x%s\n  WIF compressed: %s\n", addr, hx, wif);
    if (r->found_write_failed) printf("  NOT SAVED:      %s could not be written (see the log): copy the key from this screen now\n"
                                      "                  %s\n", r->path_found, found_again(r));
    else printf("  saved in:       %s\n", r->path_found);
    printf("=========================================================\n");
    print_spend_warning(r);
    print_found_thanks();
}

/* Two blocks of the disk kept for FOUND.txt from the first start of a campaign: a disk that fills while the search runs (its own
 * points and logs can fill it) must not cost the key. */
static void reserve_space(const run_ctx *r)
{
    static const char zero[8192];
    if (!file_exists(r->path_reserve) && pz_write_file_atomic(r->path_reserve, zero, sizeof zero, 0600) != 0)
        pz_log(PZ_LOG_WARN, "cannot write %s: %s (a disk that is full at the find would lose the key)", r->path_reserve, strerror(errno));
}

/* A campaign that has already found the key ('hunt' and 'merge' end there): FOUND.txt is shown. False when it has not. */
bool print_already_found(const run_ctx *r)
{
    if (!file_exists(r->path_found)) return false;
    char *t = pz_read_file(r->path_found, NULL);
    printf("This campaign has already found the key (%s):\n\n%s\n", r->path_found, t ? t : "");
    free(t);
    return true;
}

/* The line that tells how to resume an interrupted campaign. */
static void print_resume_hint(const run_ctx *r, const char *dir)
{
    char byp[32];
    snprintf(byp, sizeof byp, "p%d", r->cfg.puzzle);
    const bool other_dir = strcmp(dir, "campaigns") != 0;
    if (r->cfg.net)                                 /* its seed is not on disk: only the start key brings it back */
        printf("\nCampaign interrupted. pzpeer resumes it (its starts come from a start key that is not on disk).\n");
    else if (r->cfg.puzzle && !strcmp(r->cfg.name, byp))
        printf("\nCampaign interrupted. Resume with:  pzsolve hunt -p %d%s%s\n", r->cfg.puzzle, other_dir ? " -d " : "", other_dir ? dir : "");
    else
        printf("\nCampaign interrupted. Resume with:  pzsolve hunt -c %s%s%s\n", r->cfg.name, other_dir ? " -d " : "", other_dir ? dir : "");
}

/* Whether the campaign has already covered its whole range (or its whole --part): the state of its last session, if the saved state is
 * still there to show it. */
static bool campaign_completed(const run_ctx *r)
{
    char st[32];
    return file_exists(r->path_state) && !strcmp(campaign_state(r->cfg.dir, st, sizeof st), "COMPLETED");
}

/* ---- a shared walk (--jump-seed, --start-key: docs/en/NETWORK.md 6) ---- */

/* The secret of the starts, from --start-key: the seed of the campaign in memory, and only there. */
static u8 g_start_key[32];

/* --jump-seed and --start-key: a walk others can share (pzpeer starts pzsolve with them). The jumps come from the public seed, the starts
 * from the secret key, the walkers from the schema net-06, and the distinguished points are exactly those of --dp-bits. Checked first. */
static void shared_options(opts_t *o)
{
    if (!o->jump_seed[0] && !o->start_key[0]) return;
    if (!o->jump_seed[0] || !o->start_key[0]) pz_die("--jump-seed and --start-key go together");
    u8 js[32];
    if (strlen(o->jump_seed) != 64 || pz_hex_decode(o->jump_seed, js, 32) != 32) pz_die("--jump-seed: expected 64 hex digits");
    if (!o->pubkey[0] || o->dp_bits <= 0) pz_die("a shared walk needs the public key (-k) and --dp-bits");
    if (o->seed[0] || o->part[0] || o->table[0] || o->bsgs_entries || (strcmp(o->mode, "auto") && strcmp(o->mode, "kangaroo")))
        pz_die("--jump-seed and --start-key are a kangaroo of their own: -s, -m, --part, --table and --bsgs-entries are not given with them");
    snprintf(o->mode, sizeof o->mode, "kangaroo");
    char *text = pz_read_file(o->start_key, NULL);
    if (!text) pz_die("cannot read the start key %s: %s", o->start_key, strerror(errno));
    const size_t tl = strlen(text);
    char *b = text + strspn(text, " \t\r\n");
    const size_t n = strcspn(b, " \t\r\n");
    const bool blank_after = b[n + strspn(b + n, " \t\r\n")] == 0;
    b[n] = 0;
    const bool ok = n == 64 && blank_after && pz_hex_decode(b, g_start_key, 32) == 32;
    pz_secure_zero(text, tl);
    free(text);
    if (!ok) pz_die("the start key %s must hold 64 hex digits and nothing else", o->start_key);
}

/* The shared walk of the campaign: set on a new one, checked on one that resumes (the same jump seed, and a start key with the id that
 * campaign.conf keeps); then the start key becomes the seed. Such a campaign never resumes without them: its seed is not on disk. */
static void shared_bind(const opts_t *o, campaign_cfg *c, bool fresh)
{
    if (!o->jump_seed[0]) {
        if (c->net) pz_die("campaign '%s' is a shared walk: it resumes only with its --jump-seed and --start-key (pzpeer gives them)", c->name);
        return;
    }
    u8 js[32];
    char id[17];
    (void)pz_hex_decode(o->jump_seed, js, 32);
    seed_to_id(g_start_key, id);
    if (fresh) {
        c->net = true;
        memcpy(c->net_jumpseed, js, 32);
        pz_strlcpy(c->net_keyid, id, sizeof c->net_keyid);
    } else if (!c->net || memcmp(c->net_jumpseed, js, 32)) {
        pz_die("campaign '%s' is not a shared walk with this jump seed: give another name with -c", c->name);
    } else if (strcmp(c->net_keyid, id)) {
        pz_die("campaign '%s' was started with another start key (id %s, given %s): its points belong to that key", c->name, c->net_keyid, id);
    }
    memcpy(c->seed, g_start_key, 32);
}

/* The folder of a campaign is its owner's alone: its state and its seed say where the search stands, which after a find is where the
 * key is, whatever protects FOUND.txt; and a folder that another user made (in a base folder everyone can write) could hold links and
 * a configuration of theirs. */
void campaign_private(const char *dir)
{
    if (!pz_dir_private(dir)) pz_die("%s is not a folder of this user that can be closed to the others: the campaign is not opened", dir);
}

/* The campaign of the command: opened when it exists, otherwise created (its folder and campaign.conf, under its lock), with the options
 * checked against it either way. Returns whether it existed; *locked tells whether its lock is already taken. */
static bool open_or_create_campaign(opts_t *o, run_ctx *r, bool *locked)
{
    char cdir[600], conf[700];
    if (!o->campaign[0]) derive_campaign_name(o, o->campaign, sizeof o->campaign);   /* stable name: re-running resumes */
    snprintf(cdir, sizeof cdir, "%s/%s", o->dir, o->campaign);
    snprintf(conf, sizeof conf, "%s/campaign.conf", cdir);
    bool resuming = file_exists(conf);
    *locked = false;
    if (!resuming) {
        build_new_cfg(o, &r->cfg);
        shared_bind(o, &r->cfg, true);
        check_options_for_campaign(o, &r->cfg, r, true);
        warn_solved(o, &r->cfg);
        encrypt_from_options(o, &r->cfg);              /* last of the checks: it may ask for a password, and nothing is created yet */
        snprintf(r->cfg.dir, sizeof r->cfg.dir, "%s/%s", o->dir, r->cfg.name);
        if (pz_mkdir_p(r->cfg.dir) != 0) pz_die("cannot create folder %s: %s", r->cfg.dir, strerror(errno));
        campaign_private(r->cfg.dir);
        /* Two commands that start the same new campaign at the same moment both get here with a seed of their own: whoever takes the
         * lock decides, and only then is it known whether the configuration exists. Writing it before the lock would let the loser
         * leave its seed in campaign.conf while the winner checkpoints with another one. */
        campaign_lock(r->cfg.dir);   /* released at process exit */
        *locked = true;
        snprintf(conf, sizeof conf, "%s/campaign.conf", r->cfg.dir);
        if (file_exists(conf)) {      /* created by a command that has finished meanwhile: this one resumes it */
            resuming = true;
        } else if (cfg_save(&r->cfg, conf) != 0) pz_die("cannot save %s", conf);
    }
    if (resuming) {
        char err[256];
        campaign_private(cdir);
        if (cfg_load(&r->cfg, conf, err, sizeof err) != 0) pz_die("%s", err);
        shared_bind(o, &r->cfg, false);
        check_resume_consistency(o, &r->cfg);
        check_options_for_campaign(o, &r->cfg, r, false);
        pz_strlcpy(r->cfg.dir, cdir, sizeof r->cfg.dir);
    }
    return resuming;
}

/* A campaign that has covered its whole range (or its --part) has nothing left to do: one line says so. */
static void print_completed(const run_ctx *r, const char *dir)
{
    char msg[1400];
    const bool other_dir = strcmp(dir, "campaigns") != 0;
    if (r->cfg.part[0]) snprintf(msg, sizeof msg, "Part %s of campaign '%s' was already scanned in full without the key.\n", r->cfg.part, r->cfg.name);
    else snprintf(msg, sizeof msg, "Campaign '%s' is already completed: the whole range was covered without the key (see 'pzsolve info -c %s%s%s').\n",
                  r->cfg.name, r->cfg.name, other_dir ? " -d " : "", other_dir ? dir : "");
    pz_fputs_wrap(stdout, msg, PZ_LINE_MAX, 2);
}

/* The settings of the session from the options (threads, devices, display, memory, CPU engine), then the machine profile and --limit.
 * False when a Ctrl-C interrupted the measurement of the machine. *thr_full: the CPU threads before --limit. */
static bool apply_options(const opts_t *o, run_ctx *r, int *thr_full)
{
    r->nthreads = o->threads > 0 ? o->threads : r->sys.ncpu;
    if (r->nthreads > RUN_MAX_THREADS) {
        pz_log(PZ_LOG_WARN, "%d cores detected: using at most %d threads", r->nthreads, RUN_MAX_THREADS);
        r->nthreads = RUN_MAX_THREADS;
    }
    r->threads_explicit = o->threads > 0;
    r->mode_auto = !strcmp(o->mode, "auto");
    r->node = (u32)o->node;
    r->node_given = o->node_given;
    r->use_cpu = !o->no_cpu;
    r->use_gpu = !o->no_gpu;
    if (o->gpu[0]) pz_strlcpy(r->gpu_filter, o->gpu, sizeof r->gpu_filter);
    r->ui = o->quiet ? UI_LOG : o->ui;
    r->mem_limit = mem_bytes(o->mem_gb);
    pz_strlcpy(r->table_path, o->table, sizeof r->table_path);
    if (o->dp_bits >= 0) r->cfg.dp_bits = (unsigned)o->dp_bits;

    if (r->use_cpu && r->cfg.mode == MODE_LINEAR) {
        if (!isa_forced()) r->engine = cpu_engine_autotune(0.25, o->verbose);
        else {
            r->engine = cpu_engine_by_name(g_env.isa);          /* known and supported: isa_forced() has already said otherwise and exited */
            if (!cpu_engine_quickcheck(r->engine)) pz_die("the CPU engine '%s' gave a wrong result in the quick check: set PZ_ISA to another one", g_env.isa);
        }
    }

    r->prio_gpu = g_env.prio_gpu == PZ_ENV_PRIO_YES ? PRIO_YES : g_env.prio_gpu == PZ_ENV_PRIO_NO ? PRIO_NO : PRIO_AUTO;   /* PZ_PRIO_GPU */
    r->gpu_boost = r->prio_gpu == PRIO_YES;
    r->limit_pct = o->limit;
    profile_apply(r, o->retune);   /* CPU threads and GPUs: the measured best of this machine unless the user chose (nothing under --limit) */
    if (run_interrupted()) return false;    /* Ctrl-C during the measurement: nothing has started, nothing is lost */
    *thr_full = r->nthreads;
    run_limit_setup(r);
    if (r->gpu_boost && !pz_boost_possible()) {
        pz_log(PZ_LOG_WARN, "high priority for the threads that serve the GPUs: the system does not allow it for this user (on Linux it needs root or CAP_SYS_NICE): staying normal");
        r->gpu_boost = false;
    }
    gpu_set_boost(r->use_gpu && r->gpu_boost);
    if (!g_env.no_profile) profile_rate_setup(r);

    return true;
}

/* The lines that start the session in the log: version, target, range, mode, hardware and what the options changed. */
static void log_session_header(const opts_t *o, const run_ctx *r, bool resuming, int thr_full)
{
    char idb[17], rs[80], re[80], rsz[80], addr[80];
    seed_to_id(r->cfg.seed, idb);
    pz_u256_to_hex(&r->cfg.range_start, rs);
    pz_u256_to_hex(&r->cfg.range_end, re);
    pz_u256 sz = cfg_range_size(&r->cfg);
    pz_u256_to_dec(&sz, rsz);
    char rsz_short[64];
    size_text(&sz, rsz, rsz_short, sizeof rsz_short);
    pz_strlcpy(addr, r->cfg.address, sizeof addr);
    char memb[32];
    pz_fmt_bytes((double)r->sys.mem_total, memb, sizeof memb);
    pz_log(PZ_LOG_INFO, "pzsolve %s - campaign '%s' %s", PZ_VERSION, r->cfg.name, resuming ? "(resumed)" : "(new)");
    /* the header of the panel shows target, mode and seed: on a terminal with the panel these lines go to the log file only */
    void (*const say)(pz_loglevel, const char *, ...) = run_ui_is_panel(r) ? pz_log_file : pz_log;
    if (r->cfg.puzzle) say(PZ_LOG_INFO, "target: puzzle #%d  address %s", r->cfg.puzzle, addr);
    else say(PZ_LOG_INFO, "target: address %s", addr);
    pz_log(PZ_LOG_INFO, "range: 0x%s - 0x%s (%s)", rs, re, rsz_short);
    if (r->cfg.mode == MODE_LINEAR)
        say(PZ_LOG_INFO, "mode: linear | seed id %s | order: blocks of 2^%u keys permuted with the seed", idb, r->cfg.chunk_bits);
    else if (r->cfg.mode == MODE_BSGS)
        say(PZ_LOG_INFO, "mode: bsgs (known public key) | seed id %s | random order of the giant steps determined by the seed", idb);
    else if (r->cfg.net)
        say(PZ_LOG_INFO, "mode: kangaroo, shared walk (net-06) | start key id %s | public jumps, secret starts", idb);
    else
        say(PZ_LOG_INFO, "mode: kangaroo (known public key) | seed id %s | the seed determines jumps and starting positions", idb);
    if (r->cfg.part[0] && !r->cfg.net) {          /* never the start key of a shared walk (--part is refused there anyway) */
        char sd[65];
        pz_hex_encode(r->cfg.seed, 32, sd);
        const bool text_seed = o->seed[0] && strcmp(o->seed, "auto") != 0;   /* the seed the user typed is the one to give the others */
        const bool show = !text_seed && !r->cfg.has_encrypt;   /* an encrypting campaign keeps its seed out of the log (forget_seed) */
        pz_log(PZ_LOG_INFO, "part %s: this computer scans only its shares of the blocks; the other computers need the same range, --chunk-bits and %s%s%s with the other shares",
               r->cfg.part, text_seed ? "the same -s" : show ? "this seed (-s " : "the seed of this campaign", show ? sd : "", show ? ")" : "");
        if (!text_seed && !show) printf("  the seed for the other computers (shown here only, not written to the log): -s %s\n", sd);
    }
    pz_log(PZ_LOG_INFO, "hardware: %s, %d usable logical thread%s (%d physical core%s), RAM %s", r->sys.model, r->sys.ncpu, PLURAL(r->sys.ncpu), r->sys.ncore, PLURAL(r->sys.ncore), memb);
    if (r->use_cpu && r->engine) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, engine %s (%d lane%s)%s", r->nthreads, PLURAL(r->nthreads), r->engine->name, r->engine->lanes,
                                        PLURAL(r->engine->lanes), isa_forced() ? ", imposed by PZ_ISA" : "");
    else if (r->use_cpu && r->cfg.mode == MODE_KANGAROO) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, kangaroo steps with %s", r->nthreads, PLURAL(r->nthreads), kang_round_name());
    else if (r->use_cpu) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, giant steps with %s", r->nthreads, PLURAL(r->nthreads), bsgs_engine_name());
    if (r->limit_pct) {
        char cpu[120] = "", gpu[80] = "";
        if (r->use_cpu) snprintf(cpu, sizeof cpu, "CPU %d of %d thread%s", r->nthreads, thr_full, PLURAL(thr_full));
        if (r->use_gpu) snprintf(gpu, sizeof gpu, "%sGPUs at %d%% of the work-items", r->use_cpu ? ", " : "", r->limit_pct);
        pz_log(PZ_LOG_INFO, "limit %d%%: %s%s", r->limit_pct, cpu, gpu);
    }
    if (r->use_gpu && r->gpu_boost) pz_log(PZ_LOG_INFO, "GPU: the threads that serve the GPUs run at high priority%s", r->prio_gpu == PRIO_YES ? " (PZ_PRIO_GPU=yes)" : " (from the machine profile)");
    if (r->node) pz_log(PZ_LOG_INFO, "node %u: kangaroo starting points differ from those of the other computers on the same campaign", r->node);
    pz_log(PZ_LOG_INFO, "campaign folder: %s", r->cfg.dir);
}

/* An encrypting campaign that found its key leaves nothing that leads to it. The state and the points say where the search stood, and
 * with the seed, which orders the blocks, that is where the key is: whoever read the folder afterwards found the key again in a moment.
 * The seed goes first, from memory and from campaign.conf (the reports written from here on show zeros); the files that hold the
 * position go once they are closed. An earlier copy of campaign.conf still has the seed (docs/en/USAGE.md 11.5). */
void forget_seed(run_ctx *r)
{
    char conf[700];
    snprintf(conf, sizeof conf, "%s/campaign.conf", r->cfg.dir);
    pz_secure_zero(r->cfg.seed, sizeof r->cfg.seed);
    if (cfg_save(&r->cfg, conf) != 0) pz_log(PZ_LOG_ERR, "cannot rewrite %s without the seed: delete the folder once FOUND.txt is copied", conf);
}

void forget_position(const run_ctx *r)
{
    static const char *const names[] = {"state.dat", "state.dat.bak", "dp.bin", "near.txt"};   /* near misses: keys of the last blocks */
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char p[700];
        snprintf(p, sizeof p, "%s/%s", r->cfg.dir, names[i]);
        remove(p);
    }
}

/* The end of a session: the last save, the reports, the final frame, then the found key or what is left to do. Returns the exit status. */
static int end_session(run_ctx *r, const opts_t *o, int rc)
{
    double el = run_session_elapsed(r);
    /* a key that is on the screen only (FOUND.txt could not be written): the state stays as it was before the find, so that the next
     * start finds the key again instead of taking its place for searched */
    const bool unsaved = atomic_load(&r->found) && (r->found_write_failed || r->found_seal_failed);
    const bool forget = atomic_load(&r->found) && r->cfg.has_encrypt && !unsaved;
    if (forget) forget_seed(r);
    if (!unsaved && !forget && r->checkpoint && r->checkpoint(r) != 0) {
        /* the progress of this session may not be on disk: say so loudly and fail the command */
        pz_log(PZ_LOG_ERR, "final state save FAILED in %s (disk full or not writable?): the progress of the last session may be lost", r->cfg.dir);
        rc = PZ_EXIT_ERROR;
    }
    progress_info pi;
    run_final_progress(r, &pi);
    run_write_report_files(r, &pi, true);
    pz_interrupt_done();   /* state and reports are on disk: a pending console close may terminate the process */
    run_ui_final(r, &pi);
    const bool finished = r->sched && sched_finished(r->sched);
    if (r->cleanup) r->cleanup(r);
    if (r->sched) { sched_free(r->sched); r->sched = NULL; }
    if (forget) forget_position(r);
    char ds[48];
    pz_log(PZ_LOG_INFO, "session ended after %s", pz_fmt_duration(el, ds, sizeof ds));
    if (atomic_load(&r->found)) {
        print_found_banner(r);
        onfound_wait();                               /* the box is out; the program ends when the command has (at most 120 s) */
        const int st = r->found_seal_failed || r->found_write_failed ? PZ_EXIT_ERROR : PZ_EXIT_FOUND;
        pz_log_set_file(NULL);
        free(r);
        return st;
    }
    int status = PZ_EXIT_NOT_FOUND;
    if (finished) {
        if (r->cfg.part[0]) {
            char msg[PART_TEXT + 120];
            snprintf(msg, sizeof msg, "\nPart %s was scanned in full without finding the key: the shares of the other computers remain.\n", r->cfg.part);
            pz_fputs_wrap(stdout, msg, PZ_LINE_MAX, 0);
        } else {
            pz_fputs_wrap(stdout, "\nCampaign completed: the whole range was covered without the key. Check the address and the range; "
                                  "'pzsolve selftest' checks the computation.\n", PZ_LINE_MAX, 0);
        }
    } else {
        print_resume_hint(r, o->dir);
        /* a stop that no signal asked for: the workers ended on their own (every GPU failed and the CPU is off), the log says why */
        status = run_interrupted() ? PZ_EXIT_INTERRUPTED : PZ_EXIT_ERROR;
    }
    if (rc) status = PZ_EXIT_ERROR;
    pz_log_set_file(NULL);
    free(r);
    return status;
}

int cmd_hunt(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    shared_options(&o);
    pz_log_set_quiet(o.quiet);
    pz_log_set_verbose(o.verbose);
    isa_forced();                                       /* PZ_ISA: an unknown or unsupported name stops here */
    if (o.no_cpu && o.no_gpu) pz_die("--no-cpu and --no-gpu together: no compute device selected");
    if (o.retune && o.limit) pz_die("--retune measures the machine at full load: it cannot be combined with --limit");
    if (o.gpu[0] && !o.no_gpu) gpu_check_filter(o.gpu);   /* before anything is created on disk */
    if (o.encrypt_pubkey[0] || o.encrypt_password) harden_process();   /* before the password is typed or the key can be found */
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    pz_strlcpy(r->on_found, o.on_found, sizeof r->on_found);
    bool locked;
    const bool resuming = open_or_create_campaign(&o, r, &locked);
    run_paths(r);
    if (resuming && print_already_found(r)) {
        free(r);
        return PZ_EXIT_FOUND;
    }
    if (resuming && campaign_completed(r)) {
        print_completed(r, o.dir);
        free(r);
        return PZ_EXIT_NOT_FOUND;
    }
    if (!locked) campaign_lock(r->cfg.dir);   /* released at process exit */
    if (resuming) encrypt_on_resume(&o, r);
    pz_log_set_file(r->path_log);
    run_install_signals(r);   /* before the measurement of the machine, which takes minutes: a Ctrl-C ends it and cleans up instead of killing the program */
    int thr_full;
    if (!apply_options(&o, r, &thr_full)) {
        pz_log(PZ_LOG_INFO, "interrupted during the measurement of the machine: the search was not started");
        print_resume_hint(r, o.dir);
        pz_interrupt_done();
        pz_log_set_file(NULL);
        free(r);
        return PZ_EXIT_INTERRUPTED;
    }
    log_session_header(&o, r, resuming, thr_full);
    announce_encrypt(r);

    run_session_load(r);
    r->sessions++;
    /* saved at once: the number of the session seeds the streams of the walkers (and the starts of a shared walk), and a run cut before
     * its first checkpoint would leave it to the next one, which would draw the same starts again */
    if (run_session_save(r, r->elapsed_prev, r->prev_units_total) != 0 && r->cfg.net)
        pz_die("cannot save the session in %s: a shared walk would draw the same starts again at the next run", r->cfg.dir);
    reserve_space(r);
    r->t_start = pz_mono();
    if (o.near && r->cfg.mode == MODE_LINEAR) {
        char path[700];
        snprintf(path, sizeof path, "%s/near.txt", r->cfg.dir);
        if (!(r->near = pz_fopen_private(path, true))) pz_die("cannot write %s: %s", path, strerror(errno));
    }
    if (o.keep_awake && !pz_prevent_sleep(true)) pz_log(PZ_LOG_WARN, "--keep-awake: not supported on this system (on Linux use 'systemd-inhibit pzsolve hunt ...')");

    int rc = r->cfg.mode == MODE_KANGAROO ? run_kangaroo(r) : r->cfg.mode == MODE_BSGS ? run_bsgs(r) : run_linear(r);
    return end_session(r, &o, rc);
}
