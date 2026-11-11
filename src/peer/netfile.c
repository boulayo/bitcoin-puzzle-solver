/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* netfile.c - the network file (see netfile.h). A line is "key=value"; from '#' on it is a comment, and spaces at the ends of the key and of
 * the value do not count. The canonical form of a part is its lines rebuilt as "key=value", sorted bytewise and joined with '\n': the
 * same file reformatted by hand has the same fingerprint. */
#include "netfile.h"
#include <pzcore/ec.h>
#include <pzcore/hash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 256
#define MAX_LINES    96

static const char *const DEFINING[] = {"net", "puzzle", "pubkey", "range", "jumpseed", "schema", "dpbits", "chain"};
static const char *const OPTIONAL[] = {"prize", "entry", "electrum", "checkpoint"};
#define NIMM (sizeof DEFINING / sizeof *DEFINING)

static int key_index(const char *k, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!strcmp(k, list[i])) return (int)i;
    return -1;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* exactly 2n hex digits */
static bool hex_exact(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        const int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static bool small_uint(const char *s, unsigned max, unsigned *out)
{
    unsigned v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > max) return false;
    }
    *out = v;
    return true;
}

static bool name_ok(const char *s)
{
    if (!*s || strlen(s) > 31) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
    return true;
}

static int cmp_line(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* lines[0..n) sorted and joined with '\n' into out (cap bytes); false when it does not fit */
static bool join_sorted(char **lines, int n, char *out, size_t cap, size_t *len)
{
    qsort(lines, (size_t)n, sizeof *lines, cmp_line);
    size_t l = 0;
    for (int i = 0; i < n; i++) {
        const size_t k = strlen(lines[i]);
        if (l + k + 2 > cap) return false;
        if (i) out[l++] = '\n';
        memcpy(out + l, lines[i], k);
        l += k;
    }
    out[l] = 0;
    *len = l;
    return true;
}

static bool fail(char *err, size_t errlen, const char *msg, const char *what)
{
    snprintf(err, errlen, "network file: %s%s%.40s%s", msg, what ? " '" : "", what ? what : "", what ? "'" : "");
    return false;
}

/* the value of a defining key into nf */
static bool set_defining(netfile *nf, int k, const char *v, char *err, size_t errlen)
{
    unsigned u;
    pz_ge pt;
    switch (k) {
    case 0:
        if (!name_ok(v)) return fail(err, errlen, "net must be 1 to 31 lower-case letters, digits or '-', not", v);
        strcpy(nf->net, v);
        return true;
    case 1:
        if (!small_uint(v, 256, &u)) return fail(err, errlen, "puzzle must be 0 (a target of its own) to 256, not", v);
        nf->puzzle = (int)u;
        return true;
    case 2:
        if (!hex_exact(v, nf->pubkey33, 33) || !pz_ge_from_pubkey(&pt, nf->pubkey33, 33)) return fail(err, errlen, "invalid pubkey", v);
        return true;
    case 3: {
        const char *colon = strchr(v, ':');
        char a[80];
        if (!colon || (size_t)(colon - v) >= sizeof a) return fail(err, errlen, "range must be start:end in hex, not", v);
        memcpy(a, v, (size_t)(colon - v));
        a[colon - v] = 0;
        if (!pz_u256_from_hex(&nf->start, a) || !pz_u256_from_hex(&nf->end, colon + 1) || pz_u256_is_zero(&nf->start) ||
            pz_u256_cmp(&nf->start, &nf->end) > 0 || pz_u256_cmp(&nf->end, &pz_secp256k1_n) >= 0)
            return fail(err, errlen, "invalid range (1 <= start <= end < n)", v);
        return true;
    }
    case 4:
        if (!hex_exact(v, nf->jumpseed, 32)) return fail(err, errlen, "jumpseed must be 64 hex digits, not", v);
        return true;
    case 5:
        if (strcmp(v, "net-06")) return fail(err, errlen, "unknown schema (this version knows net-06)", v);
        strcpy(nf->schema, v);
        return true;
    case 6:
        if (!small_uint(v, 60, &u) || u < 8) return fail(err, errlen, "dpbits must be 8 to 60, not", v);
        nf->dpbits = u;
        return true;
    default:
        if (strcmp(v, "main") && strcmp(v, "test") && strcmp(v, "signet") && strcmp(v, "regtest"))
            return fail(err, errlen, "chain must be main, test, signet or regtest, not", v);
        strcpy(nf->chain, v);
        return true;
    }
}

bool netfile_parse(const char *text, netfile *nf, char *err, size_t errlen)
{
    memset(nf, 0, sizeof *nf);
    static const uint8_t TAG[] = "pznet1/file";
    char buf[MAX_LINES][LINE_MAX_LEN];
    char *imm[NIMM], *opt[MAX_LINES];
    int nimm = 0, nopt = 0, nlines = 0;
    bool seen[NIMM] = {false};
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[LINE_MAX_LEN];
        if (len >= sizeof line) return fail(err, errlen, "a line is too long", NULL);
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (e != NULL);
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *k = line, *eq = strchr(line, '=');
        while (*k == ' ' || *k == '\t') k++;
        size_t kl = strlen(k);
        while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t' || k[kl - 1] == '\r')) k[--kl] = 0;
        if (!kl) continue;                                     /* empty or only a comment */
        if (!eq) return fail(err, errlen, "a line without '=':", k);
        *eq = 0;
        char *v = eq + 1;
        for (size_t l = strlen(k); l && (k[l - 1] == ' ' || k[l - 1] == '\t'); ) k[--l] = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (nlines == MAX_LINES) return fail(err, errlen, "too many lines", NULL);
        char *canon = buf[nlines++];
        if (snprintf(canon, LINE_MAX_LEN, "%s=%s", k, v) >= LINE_MAX_LEN) return fail(err, errlen, "a line is too long", NULL);   /* never: it was one */
        int i;
        if ((i = key_index(k, DEFINING, NIMM)) >= 0) {
            if (seen[i]) return fail(err, errlen, "repeated key", k);
            seen[i] = true;
            if (!set_defining(nf, i, v, err, errlen)) return false;
            imm[nimm++] = canon;
        } else if (key_index(k, OPTIONAL, sizeof OPTIONAL / sizeof *OPTIONAL) >= 0) {
            opt[nopt++] = canon;
        } else {
            return fail(err, errlen, "unknown key", k);
        }
    }
    for (size_t i = 0; i < NIMM; i++) if (!seen[i]) return fail(err, errlen, "missing key", DEFINING[i]);
    char canon_imm[NIMM * LINE_MAX_LEN];
    size_t li = 0;
    (void)join_sorted(imm, nimm, canon_imm, sizeof canon_imm, &li);   /* fits: NIMM lines of LINE_MAX_LEN at most */
    uint8_t *m = malloc(sizeof TAG - 1 + li);
    if (!m) return fail(err, errlen, "out of memory", NULL);
    memcpy(m, TAG, sizeof TAG - 1);
    memcpy(m + sizeof TAG - 1, canon_imm, li);
    pz_sha256(m, sizeof TAG - 1 + li, nf->fp);
    free(m);
    if (!join_sorted(opt, nopt, nf->opts, sizeof nf->opts, &nf->opts_len)) return fail(err, errlen, "too many optional lines", NULL);
    return true;
}

bool netfile_load(const char *path, netfile *nf, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open the network file %s", path);
        return false;
    }
    const size_t cap = MAX_LINES * LINE_MAX_LEN + 1;   /* more than any valid file holds */
    char *text = malloc(cap + 1);
    if (!text) {
        fclose(f);
        return fail(err, errlen, "out of memory", NULL);
    }
    const size_t n = fread(text, 1, cap, f);
    fclose(f);
    text[n] = 0;
    const bool ok = n == cap ? fail(err, errlen, "the file is too large", NULL)
                  : memchr(text, 0, n) ? fail(err, errlen, "not a text file", NULL)
                  : netfile_parse(text, nf, err, errlen);
    free(text);
    return ok;
}
