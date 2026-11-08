/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* campaign.c - configuration persistence, seeds. */
#include "campaign.h"
#include <pzcore/hash.h>
#include <pzcore/seed.h>
#include "platform.h"
#include <time.h>
#include <errno.h>

/* ---- modes, ranges and block sizes ---- */

const char *mode_name(search_mode m) { return m == MODE_KANGAROO ? "kangaroo" : m == MODE_BSGS ? "bsgs" : "linear"; }

bool puzzle_range(int n, pz_u256 *start, pz_u256 *end)
{
    if (n < 1 || n > 256) return false;
    *start = pz_u256_pow2((unsigned)n - 1);
    pz_u256 top = pz_u256_pow2((unsigned)n);
    pz_u256 one = pz_u256_from_u64(1);
    if (n == 256) pz_u256_sub(end, &pz_secp256k1_n, &one);     /* keys stop at the group order n - 1 < 2^256 - 1 */
    else pz_u256_sub(end, &top, &one);
    return true;
}

pz_u256 cfg_range_size(const campaign_cfg *c)
{
    pz_u256 d, one = pz_u256_from_u64(1);
    pz_u256_sub(&d, &c->range_end, &c->range_start);
    pz_u256_add(&d, &d, &one);
    return d;
}

unsigned cfg_default_chunk_bits(const pz_u256 *range_size)
{
    pz_u256 m1, one = pz_u256_from_u64(1);
    pz_u256_sub(&m1, range_size, &one);
    unsigned rb = pz_u256_bits(&m1);        /* range ~ 2^rb */
    int b = (int)rb - 12;                /* at least ~4096 chunks when the range allows it */
    if (b < 12) b = 12;
    if (b > 36) b = 36;
    return (unsigned)b;
}

/* The default block size (bits) of a linear search over range_size keys split in `shares` parts (1 without --part). A launch of a GPU
 * cannot cross a block, and its controller aims at launches of about a third of a second (up to 2^33 keys on the fastest cards): blocks of
 * 2^24 keys, which the default above gave to a range of 2^36, cost a GPU about a fifth of its speed in launches too short to amortize
 * their fixed cost. So the blocks are as large as the range allows while each share keeps at least 16 of them, and never above 2^36.
 * It depends on the range and the number of shares only, never on the machine: the computers of a --part split must agree on it. */
unsigned cfg_linear_chunk_bits(const pz_u256 *range_size, unsigned shares)
{
    pz_u256 m1, one = pz_u256_from_u64(1);
    pz_u256_sub(&m1, range_size, &one);
    int b = (int)pz_u256_bits(&m1) - 4;      /* 16 blocks */
    for (unsigned s = 1; s < shares; s <<= 1) b--;   /* ... for each share: ceil(log2(shares)) more bits of blocks */
    if (b < 12) b = 12;
    if (b > 36) b = 36;
    return (unsigned)b;
}

/* ---- seeds ---- */

/* pzcore's generator (seed.h): the OS, the CPU's generator, CPU timing jitter and the TPM where there is one. A campaign seed only has
 * to be unpredictable and its own, so it takes what the OS alone gives when the others fall short (a virtual machine with a coarse
 * timer), as campaigns always did, and waits for more jitter only briefly. */
void seed_generate(u8 seed[32])
{
    const pz_seed_opts o = {.flags = PZ_SEED_ALLOW_DEGRADED, .max_ms = 200};
    pz_seed_report r;
    if (pz_seed_generate(seed, &o, &r) != PZ_SEED_OK) pz_die("cannot generate a seed: %s", r.error);
}

void seed_from_text(const char *text, u8 seed[32])
{
    u8 tmp[32];
    if (strlen(text) == 64 && pz_hex_decode(text, tmp, 32) == 32) { memcpy(seed, tmp, 32); return; }
    u8 buf[512];
    size_t n = strlen(text);
    if (n > sizeof buf - 8) n = sizeof buf - 8;
    memcpy(buf, "PZSEED1:", 8);
    memcpy(buf + 8, text, n);
    pz_sha256(buf, n + 8, seed);
}

void seed_to_id(const u8 seed[32], char id[17])
{
    u8 h[32], buf[40];
    memcpy(buf, "PZID", 4);
    memcpy(buf + 4, seed, 32);
    pz_sha256(buf, 36, h);
    pz_hex_encode(h, 8, id);
}

/* ---- the shares of --part ---- */

bool part_parse(const char *text, part_spec *out, char *err, size_t errn)
{
    memset(out, 0, sizeof *out);
    if (!text || !text[0]) return true;
    const char *slash = strchr(text, '/');
    if (!slash || slash == text || !slash[1]) { snprintf(err, errn, "expected SHARES/TOTAL, for example 0/2 or 0-6/9"); return false; }
    unsigned total = 0;
    for (const char *p = slash + 1; *p; p++) {
        if (*p < '0' || *p > '9' || total > PART_MAX_TOTAL) { snprintf(err, errn, "the total of shares must be a number between 1 and %d", PART_MAX_TOTAL); return false; }
        total = total * 10 + (unsigned)(*p - '0');
    }
    if (total < 1 || total > PART_MAX_TOTAL) { snprintf(err, errn, "the total of shares must be a number between 1 and %d", PART_MAX_TOTAL); return false; }
    bool taken[PART_MAX_TOTAL];
    memset(taken, 0, sizeof taken);
    const char *p = text;
    while (p < slash) {
        unsigned v[2] = {0, 0};
        int nv = 1;
        const char *q = p;
        bool digits = false;
        for (; q < slash && *q != ','; q++) {
            if (*q == '-' && nv == 1 && digits) { nv = 2; digits = false; continue; }
            if (*q < '0' || *q > '9' || v[nv - 1] > PART_MAX_TOTAL) { snprintf(err, errn, "invalid shares: '%.*s'", (int)(slash - text), text); return false; }
            v[nv - 1] = v[nv - 1] * 10 + (unsigned)(*q - '0');
            digits = true;
        }
        if (!digits) { snprintf(err, errn, "invalid shares: '%.*s'", (int)(slash - text), text); return false; }
        const unsigned a = v[0], b = nv == 2 ? v[1] : v[0];
        if (a > b) { snprintf(err, errn, "share range is reversed: %u-%u", a, b); return false; }
        if (b >= total) { snprintf(err, errn, "share %u does not exist: with %u shares they go from 0 to %u", b, total, total - 1); return false; }
        for (unsigned i = a; i <= b; i++) {
            if (taken[i]) { snprintf(err, errn, "share %u is listed twice", i); return false; }
            taken[i] = true;
        }
        if (q < slash && q + 1 == slash) { snprintf(err, errn, "trailing comma at the end of the list of shares"); return false; }
        p = q < slash ? q + 1 : q;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < total; i++) if (taken[i]) out->res[n++] = (unsigned short)i;
    if (n == total) return true;                       /* all of them: the whole order, no split */
    out->total = total;
    out->count = n;
    return true;
}

void part_format(const part_spec *p, char *out, size_t n)
{
    if (n == 0) return;
    out[0] = 0;
    if (!p->total) return;
    size_t o = 0;
    for (unsigned i = 0; i < p->count && o + 16 < n;) {
        unsigned j = i;
        while (j + 1 < p->count && p->res[j + 1] == p->res[j] + 1) j++;
        if (j > i) o += (size_t)snprintf(out + o, n - o, "%s%u-%u", o ? "," : "", p->res[i], p->res[j]);
        else o += (size_t)snprintf(out + o, n - o, "%s%u", o ? "," : "", p->res[i]);
        i = j + 1;
    }
    snprintf(out + o, n - o, "/%u", p->total);
}

bool part_split(const u64 *speed, unsigned n, unsigned total, unsigned *counts)
{
    if (n == 0 || n > total || total > PART_MAX_TOTAL) return false;
    u128 sum = 0;
    for (unsigned i = 0; i < n; i++) {
        if (speed[i] == 0) return false;
        sum += speed[i];
    }
    u128 rem[PART_MAX_TOTAL];
    unsigned given = 0;
    for (unsigned i = 0; i < n; i++) {
        const u128 num = (u128)speed[i] * total;
        counts[i] = (unsigned)(num / sum);
        rem[i] = num % sum;
        given += counts[i];
    }
    for (unsigned left = total - given; left > 0; left--) {     /* the leftover shares go to the largest remainders, first computer on ties */
        unsigned best = 0;
        for (unsigned i = 1; i < n; i++) if (rem[i] > rem[best]) best = i;
        counts[best]++;
        rem[best] = 0;
    }
    for (unsigned i = 0; i < n; i++) {                       /* nobody is left without a share: the largest computer gives one up */
        while (counts[i] == 0) {
            unsigned big = 0;
            for (unsigned j = 1; j < n; j++) if (counts[j] > counts[big]) big = j;
            counts[big]--;
            counts[i]++;
        }
    }
    return true;
}

/* ---- campaign.conf ---- */

int cfg_save(const campaign_cfg *c, const char *path)
{
    char buf[4096], start_hex[80], end_hex[80], hash160_hex[130], seed_hex[65], pubkey_hex[80], encrypt_pubkey_hex[67] = "";
    pz_u256_to_hex(&c->range_start, start_hex);
    pz_u256_to_hex(&c->range_end, end_hex);
    pz_hex_encode(c->target_h160, 20, hash160_hex);
    pz_hex_encode(c->seed, 32, seed_hex);
    pubkey_hex[0] = 0;
    if (c->has_pubkey) pz_hex_encode(c->pubkey33, 33, pubkey_hex);
    if (c->has_encrypt) pz_hex_encode(c->encrypt_pubkey33, 33, encrypt_pubkey_hex);
    int n = snprintf(buf, sizeof buf,
        "# pzsolve campaign (do not edit by hand)\n"
        "format=%u\n"
        "created_with=%s\n"
        "created_at=%.0f\n"
        "name=%s\n"
        "puzzle=%d\n"
        "range_start=%s\n"
        "range_end=%s\n"
        "address=%s\n"
        "hash160=%s\n"
        "pubkey=%s\n"
        "mode=%s\n"
        "seed=%s\n"
        "chunk_bits=%u\n"
        "dp_bits=%u\n"
        "bsgs_entries=%llu\n"
        "part=%s\n"
        "%s%s%s",
        c->format_version, c->created_with, c->created_at, c->name, c->puzzle, start_hex, end_hex,
        c->address, hash160_hex, pubkey_hex, mode_name(c->mode), seed_hex, c->chunk_bits, c->dp_bits, (unsigned long long)c->bsgs_t, c->part,
        c->has_encrypt ? "encrypt_pubkey=" : "", encrypt_pubkey_hex,   /* the two lines are there only when they are used: the file of a campaign without them is as before */
        c->has_encrypt ? "\nrequires=" PZ_REQUIRES_ENCRYPT "\n" : "");
    return pz_write_file_atomic(path, buf, (size_t)n, 0644);
}

int cfg_add_encrypt_pubkey(const campaign_cfg *c, const char *path)
{
    char *text = pz_read_file(path, NULL);
    if (!text) return -1;
    char hx[67];
    pz_hex_encode(c->encrypt_pubkey33, 33, hx);
    const size_t l = strlen(text), cap = l + 100;
    char *out = pz_xmalloc(cap);
    const int n = snprintf(out, cap, "%s%sencrypt_pubkey=%s\nrequires=" PZ_REQUIRES_ENCRYPT "\n", text, l && text[l - 1] != '\n' ? "\n" : "", hx);
    const int rc = pz_write_file_atomic(path, out, (size_t)n, 0644);
    free(out);
    free(text);
    return rc;
}

/* "X.Y.Z" as three numbers; false when the text does not start with three numbers separated by dots */
static bool version_parse(const char *s, unsigned long v[3])
{
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        v[i] = 0;                               /* no strtoul: on glibc >= 2.38 it would pull in __isoc23_strtoul (common.h) */
        while (*s >= '0' && *s <= '9' && v[i] < 100000000UL) v[i] = v[i] * 10 + (unsigned long)(*s++ - '0');
        if (i < 2) { if (*s != '.') return false; s++; }
    }
    return true;
}

int cfg_version_cmp(const char *a, const char *b)
{
    unsigned long va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
    (void)version_parse(a, va);
    (void)version_parse(b, vb);
    for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

static const char *cfg_val(char *text, const char *key, char *out, size_t outlen)
{
    const char *v = pz_kv_find(text, key, '=', 0);
    if (!v) return NULL;
    size_t len = strcspn(v, "\n");
    if (len >= outlen) len = outlen - 1;
    memcpy(out, v, len);
    out[len] = 0;
    return out;
}

int cfg_load(campaign_cfg *c, const char *path, char *err, size_t errlen)
{
    char *text = pz_read_file(path, NULL);
    if (!text) { snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno)); return -1; }
    char v[256];
    memset(c, 0, sizeof *c);
#define GET(k) if (!cfg_val(text, k, v, sizeof v)) { snprintf(err, errlen, "missing key '%s' in %s", k, path); free(text); return -1; }
    GET("format") c->format_version = (u32)pz_strtou64(v);
    if (c->format_version != PZ_FORMAT_VERSION) {
        snprintf(err, errlen, "campaign format %u not supported by this version (expected %u)", c->format_version, PZ_FORMAT_VERSION);
        free(text);
        return -1;
    }
    {
        char rv[40];   /* the version the campaign needs (absent before 1.22.0 and in a campaign that encrypts nothing): an older version must not
                        * resume the campaign, because it would ignore what it does not know (encrypt_pubkey: the key would be written in plain) */
        if (cfg_val(text, "requires", rv, sizeof rv) && rv[0]) {
            unsigned long dummy[3];
            if (!version_parse(rv, dummy)) {
                snprintf(err, errlen, "invalid requires '%.20s' in %s (expected a version X.Y.Z)", rv, path);
                free(text);
                return -1;
            }
            if (cfg_version_cmp(PZ_VERSION, rv) < 0) {
                snprintf(err, errlen, "this campaign needs pzsolve %s or later: it was made with %s (this is pzsolve %s)", rv,
                         pz_kv_find(text, "encrypt_pubkey", '=', 0) ? "--encrypt-pubkey" : "a feature this version does not have", PZ_VERSION);
                free(text);
                return -1;
            }
        }
    }
    GET("created_with") pz_strlcpy(c->created_with, v, sizeof c->created_with);
    GET("created_at") c->created_at = atof(v);
    GET("name") pz_strlcpy(c->name, v, sizeof c->name);
    GET("puzzle") c->puzzle = pz_atoi(v);
    GET("range_start") if (!pz_u256_from_hex(&c->range_start, v)) { snprintf(err, errlen, "invalid range_start"); free(text); return -1; }
    GET("range_end") if (!pz_u256_from_hex(&c->range_end, v)) { snprintf(err, errlen, "invalid range_end"); free(text); return -1; }
    GET("address") pz_strlcpy(c->address, v, sizeof c->address);
    GET("hash160") if (pz_hex_decode(v, c->target_h160, 20) != 20) { snprintf(err, errlen, "invalid hash160"); free(text); return -1; }
    GET("pubkey")
    if (v[0]) {
        if (pz_hex_decode(v, c->pubkey33, 33) != 33 || !pz_ge_from_pubkey(&c->pubkey, c->pubkey33, 33)) {
            snprintf(err, errlen, "invalid pubkey");
            free(text);
            return -1;
        }
        c->has_pubkey = true;
    }
    GET("mode")
    if (!strcmp(v, "linear")) c->mode = MODE_LINEAR;
    else if (!strcmp(v, "kangaroo")) c->mode = MODE_KANGAROO;
    else if (!strcmp(v, "bsgs")) c->mode = MODE_BSGS;
    else {            /* not guessed: a search of another kind than the campaign's would run on its state */
        snprintf(err, errlen, "unknown mode '%.20s' in %s (a campaign of another version?)", v, path);
        free(text);
        return -1;
    }
    GET("seed") if (pz_hex_decode(v, c->seed, 32) != 32) { snprintf(err, errlen, "invalid seed"); free(text); return -1; }
    GET("chunk_bits") c->chunk_bits = (unsigned)pz_strtou64(v);
    GET("dp_bits") c->dp_bits = (unsigned)pz_strtou64(v);
    if (cfg_val(text, "bsgs_entries", v, sizeof v)) c->bsgs_t = pz_strtou64(v);   /* absent in campaigns created by 1.0.x */
    {
        char pv[PART_TEXT];                                                           /* absent before 1.7.5: the whole order */
        if (cfg_val(text, "part", pv, sizeof pv) && pv[0]) {
            part_spec ps;
            if (!part_parse(pv, &ps, v, sizeof v)) { snprintf(err, errlen, "invalid part in %s: %s", path, v); free(text); return -1; }
            part_format(&ps, c->part, sizeof c->part);
        }
    }
    {
        char ev[80];                                                                  /* absent in the campaigns of earlier versions and in one that does not encrypt the key */
        if (cfg_val(text, "encrypt_pubkey", ev, sizeof ev) && ev[0]) {
            if (pz_hex_decode(ev, c->encrypt_pubkey33, 33) != 33 || !pz_ge_from_pubkey(&c->encrypt_pubkey, c->encrypt_pubkey33, 33)) {
                snprintf(err, errlen, "invalid encrypt_pubkey in %s (the key would not be encrypted: the campaign is not started)", path);
                free(text);
                return -1;
            }
            c->has_encrypt = true;
        }
    }
#undef GET
    free(text);
    return 0;
}
