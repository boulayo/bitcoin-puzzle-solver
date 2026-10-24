/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* campaign.c - configuration persistence, seeds. */
#include "campaign.h"
#include <pzcore/hash.h>
#include "platform.h"
#include <time.h>
#include <errno.h>

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

void seed_generate(u8 seed[32])
{
    u8 pool[32 + 8 + 8 + 8 + 64];
    memset(pool, 0, sizeof pool);
    size_t n = 0;
    if (!pz_os_random(pool, 32)) pz_die("cannot get entropy from the system");
    n = 32;
    double t = pz_now(), m = pz_mono();   /* extra, non-secret diversity: time, process, host */
    memcpy(pool + n, &t, 8); n += 8;
    memcpy(pool + n, &m, 8); n += 8;
    u64 pid = (u64)pz_getpid();
    memcpy(pool + n, &pid, 8); n += 8;
    pz_hostname((char *)pool + n, 64);
    n += 64;
    pz_sha256(pool, n, seed);
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

int cfg_save(const campaign_cfg *c, const char *path)
{
    char buf[4096], t1[80], t2[80], h[130], sd[65], pk[80];
    pz_u256_to_hex(&c->range_start, t1);
    pz_u256_to_hex(&c->range_end, t2);
    pz_hex_encode(c->target_h160, 20, h);
    pz_hex_encode(c->seed, 32, sd);
    pk[0] = 0;
    if (c->has_pubkey) pz_hex_encode(c->pubkey33, 33, pk);
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
        "part=%s\n",
        c->format_version, c->created_with, c->created_at, c->name, c->puzzle, t1, t2,
        c->address, h, pk, mode_name(c->mode), sd, c->chunk_bits, c->dp_bits, (unsigned long long)c->bsgs_t, c->part);
    return pz_write_file_atomic(path, buf, (size_t)n, 0644);
}

static const char *cfg_val(char *text, const char *key, char *out, size_t outlen)
{
    size_t kl = strlen(key);
    for (char *p = text; p && *p;) {
        char *nl = strchr(p, '\n');
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t len = nl ? (size_t)(nl - (p + kl + 1)) : strlen(p + kl + 1);
            if (len >= outlen) len = outlen - 1;
            memcpy(out, p + kl + 1, len);
            out[len] = 0;
            return out;
        }
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
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
    GET("mode") c->mode = strcmp(v, "kangaroo") == 0 ? MODE_KANGAROO : strcmp(v, "bsgs") == 0 ? MODE_BSGS : MODE_LINEAR;
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
#undef GET
    free(text);
    return 0;
}
