/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_hunt.c - 'pzsolve hunt': the campaign of the options (a new one, or the one that exists with the options checked against it),
 * the session (the settings, the log header, the run, the end with its reports) and the key when it is found; the encryption of the key
 * and the banner are shared with 'merge' (cmd.h). */
#include "cmd.h"
#include "gpu.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <pzcore/seed.h>
#include "profile.h"
#include "kangaroo.h"
#include "puzzles.h"
#include "campaigns.h"
#include "bsgs.h"
#include "platform.h"
#include "envopts.h"
#include "encrypt.h"
#include "onfound.h"
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* --seed-file: the first line of the file, or of the standard input with "-" (without echo on a terminal), becomes the text of -s, so that the
 * seed stays out of the shell history and of the list of processes. Only that line is read from the standard input. */
static void seed_from_file(opts_t *o)
{
    const char *f = o->seed_file;
    if (!f[0]) return;
    if (o->seed[0]) pz_die("-s and --seed-file give the same seed: use one of them");
    int n;
    if (!strcmp(f, "-")) {
        if (o->encrypt_password) pz_die("--seed-file - and --encrypt-password both read the standard input: give the seed in a file");
        n = pz_read_secret("Seed: ", o->seed, sizeof o->seed, false);
    } else {
        char *text = pz_read_file(f, NULL);
        if (!text) pz_die("cannot read the seed %s: %s", f, strerror(errno));
        const size_t len = strlen(text), line = strcspn(text, "\r\n");
        n = line < sizeof o->seed ? (int)line : -2;
        if (n > 0) memcpy(o->seed, text, line), o->seed[line] = 0;
        pz_secure_zero(text, len);
        free(text);
    }
    if (n == -2) pz_die("--seed-file %s: the seed is too long (at most %zu characters)", f, sizeof o->seed - 1);
    if (n <= 0) pz_die("--seed-file %s: no seed (the first line is empty, or there is none)", f);
}

/* PZ_ISA: the CPU engine it imposes, NULL when it imposes none ("auto" and unset). Read once, before a campaign directory is created, so
 * that a typo leaves nothing behind. */
static const cpu_engine *g_isa;

static void isa_read(void)
{
    const char *isa = g_env.isa;
    if (!isa[0] || !strcmp(isa, "auto")) return;
    if (!(g_isa = cpu_engine_by_name(isa))) pz_die("PZ_ISA: invalid value '%s' (expected %s)", isa, PZ_ISA_LIST);
    if (!g_isa->supported()) pz_die("PZ_ISA=%s: the CPU does not support this instruction set (see 'pzsolve devices')", isa);
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

/* Name used when -c is absent and the campaign is not the pN of a puzzle (parse_opts). It depends only on what identifies the search
 * (puzzle, range, step, target), so repeating the same command resumes the campaign instead of starting a new one. */
static void derive_campaign_name(const opts_t *o, char *out, size_t n)
{
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
    /* the empty field is where a hash160 target went until 1.22.0: kept, so that the names of the campaigns made since then stay the same;
     * a step is added only when there is one, for the same reason */
    char step[100] = "";
    pz_u256 st;
    if (o->step[0] && pz_u256_from_hex(&st, o->step)) {
        char hs[PZ_U256_HEX_BUF];
        snprintf(step, sizeof step, "|step=%s", pz_u256_to_hex(&st, hs));
    }
    int kl = snprintf(key, sizeof key, "PZNAME1|%d|%s|%s||%s%s", o->puzzle, rng, o->address, o->pubkey, step);
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
static unsigned effective_chunk_bits(const pz_u256 *rsz, int requested)
{
    return cfg_clamp_chunk_bits(rsz, requested > 0 ? (unsigned)requested : cfg_default_chunk_bits(rsz));
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
    pz_u256 pk, i;
    if ((o->range[0] || o->step[0]) && pz_u256_from_hex(&pk, db->privkey) && !cfg_index_of(c, &pk, &i))
        pz_log(PZ_LOG_WARN, "the known key of puzzle %d is not among the keys given: the campaign will not find it", db->n);
}

/* The target of a new campaign (c->target_h160, c->address, the public key): from the options, or from the database for -p N. */
static void set_target(const opts_t *o, campaign_cfg *c)
{
    bool have_target = false;
    const puzzle_info *db = o->puzzle ? puzzle_find(o->puzzle) : NULL;
    bool from_db = false;
    char err[200];
    /* every address, public key and known key of the built-in database, against each other: a damaged program stops here */
    if (o->puzzle && puzzles_verify(err, sizeof err)) pz_die("puzzle database corrupted: %s", err);
    if (db && !o->address[0] && !o->pubkey[0]) {
        /* target taken from the built-in database */
        pz_address_to_hash160(db->address, c->target_h160);
        pz_strlcpy(c->address, db->address, sizeof c->address);
        have_target = true;
        from_db = true;
        if (db->pubkey[0]) parse_pubkey(db->pubkey, c);
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
        pz_address_to_hash160(retired, c->target_h160);
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
    if (mode_from_name(o->mode, &c->mode)) {                 /* -m given (parse_opts took nothing else but "auto") */
        if (c->mode != MODE_LINEAR && !c->has_pubkey) no_pubkey(c, o->mode);
        if (c->mode == MODE_BSGS) c->bsgs_t = pick_bsgs_entries(o, rsz, NULL);
        return;
    }
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
}

/* --step: the keys of the range become START + i * STEP (cfg_set_step). A step of 1 is the range as it is. */
static void set_step(const opts_t *o, campaign_cfg *c)
{
    pz_u256 st;
    const char *why;
    pz_ge q;
    if (!o->step[0]) return;
    if (!pz_u256_from_hex(&st, o->step)) pz_die("--step: a hexadecimal number is expected");
    if (o->jump_seed[0]) pz_die("--step cannot be used in a shared walk (--jump-seed)");
    if (pz_u256_cmp(&st, &(pz_u256){{1, 0, 0, 0}}) == 0) return;
    const pz_u256 a = c->range_start, b = c->range_end;
    if (!cfg_set_step(c, &a, &b, &st, &why)) pz_die("%s", why);
    if (c->has_pubkey && !cfg_search_pubkey(c, &q)) pz_die("the public key is that of START - STEP, which is not one of the keys of the steps");
    if (pz_u256_cmp(&c->range_end, &c->range_start) == 0) pz_log(PZ_LOG_WARN, "--step is larger than the range: only its first key is searched");
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
    set_step(o, c);

    if (o->seed[0]) seed_from_text(o->seed, c->seed);    /* 64 hex digits, or a text that is hashed; without -s the campaign draws one */
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
        /* the computers of a split share out one order of the blocks: a seed drawn here would be this computer's alone */
        if (!o->seed[0])
            pz_die("--part needs -s, the same on every computer of the split (64 hexadecimal digits, or any text): with a seed of its own each "
                   "computer would visit the blocks in another order, and their shares would overlap and leave blocks that none of them scans");
        pz_strlcpy(c->part, o->part, sizeof c->part);
    }
    if (c->mode == MODE_LINEAR) {                 /* before anything is created on disk: the order of visit must exist and every share must hold a block */
        char perr[300];
        if (o->chunk_bits <= 0) {                 /* the default block of the linear search depends on the number of shares too (campaign.c) */
            part_spec ps;
            unsigned shares = c->part[0] && part_parse(c->part, &ps, perr, sizeof perr) && ps.total ? ps.total : 1;
            c->chunk_bits = cfg_clamp_chunk_bits(&rsz, cfg_linear_chunk_bits(&rsz, shares));
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
    if (o->range[0] || o->step[0] || (o->puzzle && c->has_step)) {   /* the keys the options name, as a new campaign would take them */
        campaign_cfg t = {0};
        const char *why;
        if (o->range[0] && !range_parse(o->range, &t.range_start, &t.range_end, &why)) pz_die("%s", why);
        if (!o->range[0] && (!o->puzzle || !puzzle_range(o->puzzle, &t.range_start, &t.range_end)))
            pz_die("--step goes with the range of the campaign: give it with -r or -p");
        pz_u256 st;
        if (o->step[0] && !pz_u256_from_hex(&st, o->step)) pz_die("--step: a hexadecimal number is expected");
        if (o->step[0] && pz_u256_cmp(&st, &(pz_u256){{1, 0, 0, 0}}) != 0) {
            const pz_u256 a = t.range_start, b = t.range_end;
            if (!cfg_set_step(&t, &a, &b, &st, &why)) pz_die("%s", why);
        }
        if (t.has_step != c->has_step || pz_u256_cmp(&t.range_start, &c->range_start) || pz_u256_cmp(&t.range_end, &c->range_end) ||
            (t.has_step && (pz_u256_cmp(&t.step, &c->step) || pz_u256_cmp(&t.step_base, &c->step_base))))
            pz_die("%s does not match the keys of the existing campaign (they cannot be changed; use another name with -c)",
                   o->range[0] && o->step[0] ? "--range with --step" : o->range[0] ? "--range" : o->step[0] ? "--step" : "-p without its --step");
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
    if (o->seed[0]) {
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
    search_mode want;
    if (mode_from_name(o->mode, &want) && want != c->mode)             /* "auto" names none */
        pz_die("the existing campaign uses mode %s, not %s (the mode cannot be changed: use another name with -c)", mode_name(c->mode), o->mode);
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
    if (o->near && c->mode != MODE_LINEAR)
        pz_die("--near is only used in linear search (-m linear): the kangaroo and BSGS do not try every key, they have no near misses");
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
        snprintf(out, n, "%s key%s", pz_group_digits(dec, g, sizeof g), strcmp(dec, "1") ? "s" : "");
    } else if (bits == 1) snprintf(out, n, "2^%d keys", top);
    else snprintf(out, n, "about %c.%c%ce%zu keys", dec[0], dec[1], dec[2], len - 1);
}

/* ---- encrypting the key that is found (docs/en/USAGE.md, "Encrypting the key") ---- */

/* Core dumps and same-user ptrace off, once: the plain key is in memory for a moment when it is found, and the password while it is typed. */
void harden_process(void)
{
    static bool done;
    if (done) return;
    done = true;
    if (!pz_seed_harden())
        pz_log(PZ_LOG_WARN, "core dumps could not be turned off (Windows gives a program no way to): a crash may write the memory of the program to a file");
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
}

/* What a campaign that encrypts the key says at the start of a session. */
void announce_encrypt(const run_ctx *r)
{
    if (!r->cfg.has_encrypt) return;
    harden_process();
    char id[9];
    enc_key_id(&r->cfg.encrypt_pubkey, id);
    pz_log(PZ_LOG_INFO, "the key will be encrypted to the public key with id %s when found: the screen and FOUND.txt will carry only the encrypted line", id);
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

/* what becomes of a key that could not be saved: the state stays as it was before the find (end_session), and the kangaroo's dp.bin has the
 * two points that meet (kang_table_insert) */
static const char *found_again(const run_ctx *r)
{
    return r->cfg.mode == MODE_KANGAROO ? "(the same command finds it again in dp.bin, if the disk took the points that meet)"
                                        : "(its place stays unsearched in the campaign: the same command finds it again)";
}

/* The banner printed when the key is found (by 'hunt' or by 'merge'). */
void print_found_banner(const run_ctx *r)
{
    const char *addr = r->cfg.address;
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
    char byp[32], node[32] = "";
    snprintf(byp, sizeof byp, "p%d", r->cfg.puzzle);
    if (r->node) snprintf(node, sizeof node, " --node %u", r->node);   /* without it this computer would be node 0, and repeat its starts */
    const bool other_dir = strcmp(dir, "campaigns") != 0;
    if (r->cfg.net)                                 /* its seed is not on disk: only the start key brings it back */
        printf("\nCampaign interrupted. pzpeer resumes it (its starts come from a start key that is not on disk).\n");
    else if (r->cfg.puzzle && !strcmp(r->cfg.name, byp))
        printf("\nCampaign interrupted. Resume with:  pzsolve hunt -p %d%s%s%s\n", r->cfg.puzzle, other_dir ? " -d " : "", other_dir ? dir : "", node);
    else
        printf("\nCampaign interrupted. Resume with:  pzsolve hunt -c %s%s%s%s\n", r->cfg.name, other_dir ? " -d " : "", other_dir ? dir : "", node);
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
        r->lock = campaign_lock(r->cfg.dir);
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
    cpu_basis_set(&r->cfg);                       /* --step: before the tables of the linear engines and kernels */
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
        r->engine = g_isa ? g_isa : cpu_engine_autotune(0.25, o->verbose);
        if (g_isa && !cpu_engine_quickcheck(g_isa)) pz_die("the CPU engine '%s' gave a wrong result in the quick check: set PZ_ISA to another one", g_isa->name);
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
static void log_session_header(const run_ctx *r, bool resuming, int thr_full)
{
    char idb[17], rng[3 * PZ_U256_HEX_BUF + 40], rsz[80], addr[80];
    seed_to_id(r->cfg.seed, idb);
    cfg_range_text(&r->cfg, rng, sizeof rng);
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
    pz_log(PZ_LOG_INFO, "range: %s (%s)", rng, rsz_short);
    if (r->cfg.mode == MODE_LINEAR)
        say(PZ_LOG_INFO, "mode: linear | seed id %s | order: blocks of 2^%u keys permuted with the seed", idb, r->cfg.chunk_bits);
    else if (r->cfg.mode == MODE_BSGS)
        say(PZ_LOG_INFO, "mode: bsgs (known public key) | seed id %s | random order of the giant steps determined by the seed", idb);
    else if (r->cfg.net)
        say(PZ_LOG_INFO, "mode: kangaroo, shared walk (net-06) | start key id %s | public jumps, secret starts", idb);
    else
        say(PZ_LOG_INFO, "mode: kangaroo (known public key) | seed id %s | the seed determines jumps and starting positions", idb);
    if (r->cfg.part[0]) {                        /* the seed is the user's own (-s, required with --part): it is never shown, its split id is */
        char sid[9];
        cfg_split_id(&r->cfg, sid);
        pz_log(PZ_LOG_INFO, "part %s, split id %s: this computer scans only its shares of the blocks; every computer of the split must show this "
               "split id (the same target, range, --step, --chunk-bits, number of shares and -s), with shares of its own", r->cfg.part, sid);
    }
    pz_log(PZ_LOG_INFO, "hardware: %s, %d usable logical thread%s (%d physical core%s), RAM %s", r->sys.model, r->sys.ncpu, PLURAL(r->sys.ncpu), r->sys.ncore, PLURAL(r->sys.ncore), memb);
    if (r->use_cpu && r->engine) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, engine %s (%d lane%s)%s", r->nthreads, PLURAL(r->nthreads), r->engine->name, r->engine->lanes,
                                        PLURAL(r->engine->lanes), g_isa ? ", imposed by PZ_ISA" : "");
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

/* One pass over the folder of a campaign: every file removed but FOUND.txt and campaign.conf. How many were; with `say`, one that could
 * not be removed is reported. */
static int remove_others(const char *dir, bool say)
{
    int n = 0;
    DIR *d = opendir(dir);
    if (!d && say) pz_log(PZ_LOG_ERR, "cannot read the folder %s: %s (delete all of it but FOUND.txt: it leads to the key)", dir, strerror(errno));
    for (struct dirent *e; d && (e = readdir(d));) {
        char p[1400];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "FOUND.txt") || !strcmp(e->d_name, "campaign.conf")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (remove(p) == 0) n++;
        else if (say) pz_log(PZ_LOG_ERR, "cannot delete %s: %s (delete it: it may lead to the key)", p, strerror(errno));
    }
    if (d) closedir(d);
    return n;
}

/* An encrypting campaign that found its key keeps FOUND.txt and its configuration without the seed, and nothing else. Its state, its
 * points, its reports and its log say where the search stood, and with the seed, which orders the blocks, that is where the key is:
 * whoever read the folder afterwards found the key again in a moment. The seed goes from memory and from campaign.conf; then every
 * other file of the folder goes, whatever it is, so that no name can be missing from a list. The caller has closed the files of the
 * search (Windows deletes no file that is open); the log and the lock are closed here. Another copy of the seed (an earlier copy of
 * the folder, the other computers of a --part split, a shell history) still has it (docs/en/USAGE.md 11.5). */
void forget_search(run_ctx *r)
{
    char conf[700];
    snprintf(conf, sizeof conf, "%s/campaign.conf", r->cfg.dir);
    pz_secure_zero(r->cfg.seed, sizeof r->cfg.seed);
    if (cfg_save(&r->cfg, conf) != 0) pz_log(PZ_LOG_ERR, "cannot rewrite %s without the seed: delete the folder once FOUND.txt is copied", conf);
    pz_log_set_file(NULL);
    if (r->lock > 0) close(r->lock), r->lock = 0;
    while (remove_others(r->cfg.dir, false) > 0) {}        /* again while a pass removes something: a removal can make a pass skip a file */
    remove_others(r->cfg.dir, true);
}

/* The end of a command that installed the signals: no signal, and no end of the input of --stop-with-input (its thread is detached), may
 * reach the run once it is freed. Returns status. */
static int release_run(run_ctx *r, int status)
{
    run_install_signals(NULL);
    pz_log_set_file(NULL);
    free(r);
    return status;
}

/* The end of a session: the last save, the reports, the final frame, then the found key or what is left to do. Returns the exit status. */
static int end_session(run_ctx *r, const opts_t *o, int rc)
{
    double el = run_session_elapsed(r);
    /* a key that is on the screen only (FOUND.txt could not be written): the state stays as it was before the find, so that the next
     * start finds the key again instead of taking its place for searched */
    const bool unsaved = atomic_load(&r->found) && (r->found_write_failed || r->found_seal_failed);
    const bool forget = atomic_load(&r->found) && r->cfg.has_encrypt && !unsaved;
    if (!unsaved && !forget && r->checkpoint && r->checkpoint(r) != 0) {
        /* the progress of this session may not be on disk: say so loudly and fail the command */
        pz_log(PZ_LOG_ERR, "final state save FAILED in %s (disk full or not writable?): the progress of the last session may be lost", r->cfg.dir);
        rc = PZ_EXIT_ERROR;
    }
    progress_info pi;
    run_final_progress(r, &pi);
    if (!forget) run_write_report_files(r, &pi, true);   /* forget: no report, and no last frame on the screen, of how far the search got */
    pz_interrupt_done();   /* state and reports are on disk: a pending console close may terminate the process */
    if (!forget) run_ui_final(r, &pi);
    const bool finished = r->sched && sched_finished(r->sched);
    if (r->cleanup) r->cleanup(r);
    if (r->sched) { sched_free(r->sched); r->sched = NULL; }
    if (r->near) fclose(r->near), r->near = NULL;     /* closed first: Windows deletes no file that is open */
    if (forget) forget_search(r);
    char ds[48];
    pz_log(PZ_LOG_INFO, "session ended after %s", pz_fmt_duration(el, ds, sizeof ds));
    if (atomic_load(&r->found)) {
        print_found_banner(r);
        onfound_wait();                               /* the box is out; the program ends when the command has (at most 120 s) */
        return release_run(r, r->found_seal_failed || r->found_write_failed ? PZ_EXIT_ERROR : PZ_EXIT_FOUND);
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
    return release_run(r, rc ? PZ_EXIT_ERROR : status);
}

int cmd_hunt(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    seed_from_file(&o);
    if (!strcmp(o.seed, "auto")) pz_die("-s auto: 'auto' is no longer a seed (it meant no -s): leave -s out, and a new campaign draws its seed at random");
    shared_options(&o);
    pz_log_set_quiet(o.quiet);
    pz_log_set_verbose(o.verbose);
    isa_read();                                         /* PZ_ISA: an unknown or unsupported name stops here */
    if (o.no_cpu && o.no_gpu) pz_die("--no-cpu and --no-gpu together: no compute device selected");
    if (o.retune && o.limit) pz_die("--retune measures the machine at full load: it cannot be combined with --limit");
    if (o.gpu[0] && !o.no_gpu) gpu_check_filter(o.gpu);   /* before anything is created on disk */
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
    if (!locked) r->lock = campaign_lock(r->cfg.dir);
    if (resuming) encrypt_on_resume(&o, r);
    pz_log_set_file(r->path_log);
    run_install_signals(r);
    if (o.stop_input) run_stop_with_input();
    /* the signals before the measurement of the machine, which takes minutes: a Ctrl-C ends it and cleans up instead of killing the program */
    int thr_full;
    if (!apply_options(&o, r, &thr_full)) {
        pz_log(PZ_LOG_INFO, "interrupted during the measurement of the machine: the search was not started");
        print_resume_hint(r, o.dir);
        pz_interrupt_done();
        return release_run(r, PZ_EXIT_INTERRUPTED);
    }
    log_session_header(r, resuming, thr_full);
    announce_encrypt(r);

    run_session_load(r);
    r->sessions++;
    /* saved at once: the number of the session seeds the streams of the walkers (and the starts of a shared walk), and a run cut before
     * its first checkpoint would leave it to the next one, which would draw the same starts again */
    if (run_session_save(r, r->elapsed_prev, r->prev_units_total) != 0 && r->cfg.net)
        pz_die("cannot save the session in %s: a shared walk would draw the same starts again at the next run", r->cfg.dir);
    reserve_space(r);
    r->t_start = pz_mono();
    if (o.near) {                                   /* a linear search (check_options_for_campaign) */
        char path[700];
        snprintf(path, sizeof path, "%s/near.txt", r->cfg.dir);
        if (r->prev_units_total && !file_exists(path))
            pz_log(PZ_LOG_WARN, "--near: the keys scanned before this session left no near misses (%s is new): only what is scanned from now "
                   "on writes them", path);
        if (!(r->near = pz_fopen_private(path, true))) pz_die("cannot write %s: %s", path, strerror(errno));
    }
    if (o.keep_awake && !pz_prevent_sleep(true)) pz_log(PZ_LOG_WARN, "--keep-awake: not supported on this system (on Linux use 'systemd-inhibit pzsolve hunt ...')");

    int rc = r->cfg.mode == MODE_KANGAROO ? run_kangaroo(r) : r->cfg.mode == MODE_BSGS ? run_bsgs(r) : run_linear(r);
    return end_session(r, &o, rc);
}
