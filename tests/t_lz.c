/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_lz.c - the compression of the embedded GPU kernel texts: the decoder (src/lz.c) against the encoder (tools/embed.c, included here).
 * Round trips of the real PTX modules and of random and adversarial data, with and without a dictionary; streams built by hand and at
 * random that pin the format down independently of the encoder; truncated, extended, corrupted and random streams must be rejected (or
 * give the original text back, when a change does not alter it) and nothing may touch memory outside the buffers: run it under
 * ASan/UBSan (tools/verify.sh does). Deterministic: a fixed-seed generator. Run it from the root of the repository (the PTX modules
 * are read from src/kernels/ptx). */
#define EMBED_NO_MAIN
#include "../src/lz.h"
#include "../tools/embed.c"
#include <dirent.h>

#ifndef PTX_DIR
#define PTX_DIR "src/kernels/ptx"
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint64_t rs = 0x0123456789ABCDEFULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

/* an FNV-1a of our own, to build streams by hand */
static uint32_t fnv(const unsigned char *p, size_t n) { uint32_t h = 2166136261u; while (n--) h = (h ^ *p++) * 16777619u; return h; }
static void put32(unsigned char *p, uint32_t h) { for (int k = 0; k < 4; k++) p[k] = (unsigned char)(h >> (8 * k)); }

static unsigned char *clone(const unsigned char *p, size_t n)
{
    unsigned char *q = xrealloc(NULL, n);
    if (n) memcpy(q, p, n);
    return q;
}

static unsigned char *gen_random(size_t n)
{
    unsigned char *b = xrealloc(NULL, n);
    for (size_t k = 0; k < n; k++) b[k] = (unsigned char)rnd();
    return b;
}

/* text of n bytes made of fresh random bytes (from `alphabet` values) and copies of earlier parts: short and long, near and far, overlapping */
static unsigned char *gen_mixed(size_t n, unsigned alphabet)
{
    unsigned char *b = xrealloc(NULL, n);
    size_t o = 0;
    while (o < n) {
        size_t run = 1 + rnd() % 40, k;
        if (o < 8 || rnd() % 3 == 0) {
            for (k = 0; k < run && o < n; k++) b[o++] = (unsigned char)(rnd() % alphabet);
        } else {
            size_t off = 1 + rnd() % (rnd() % 4 == 0 ? o : (o < 300 ? o : 300));
            if (rnd() % 5 == 0) run += rnd() % 3000;
            for (k = 0; k < run && o < n; k++, o++) b[o] = b[o - off];
        }
    }
    return b;
}

/* Unpacks into a buffer with guard bytes on both sides and room for `room` bytes after the dictionary; checks that nothing outside
 * [dlen, dlen + room) was written. Returns what pz_lz_unpack returned; *out is a copy of the unpacked text (NULL on an error). */
static size_t unpack_guarded(const unsigned char *z, size_t zn, const unsigned char *dict, size_t dlen, size_t room, unsigned char **out)
{
    enum { G = 64 };
    const size_t cap = dlen + room;
    unsigned char *buf = xrealloc(NULL, cap + 2 * G);
    memset(buf, 0xA5, cap + 2 * G);
    if (dlen) memcpy(buf + G, dict, dlen);
    const size_t r = pz_lz_unpack(z, zn, buf + G, dlen, cap);
    int intact = 1;
    for (size_t k = 0; k < G; k++) intact &= buf[k] == 0xA5 && buf[G + cap + k] == 0xA5;
    if (dlen) intact &= memcmp(buf + G, dict, dlen) == 0;
    CHECK(intact);
    CHECK(r == PZ_LZ_ERR || r <= room);
    *out = r == PZ_LZ_ERR ? NULL : clone(buf + G + dlen, r);
    free(buf);
    return r;
}

/* the stream is refused, or it gives the text back (a change that does not alter the text, such as another distance into a run of equal bytes) */
static void rejected_or_same(const unsigned char *z, size_t zn, const unsigned char *dict, size_t dlen, const unsigned char *text, size_t n)
{
    unsigned char *out;
    const size_t r = unpack_guarded(z, zn, dict, dlen, n, &out);
    CHECK(r == PZ_LZ_ERR || (r == n && (!n || memcmp(out, text, n) == 0)));
    free(out);
}

/* encode, unpack and compare; returns the stream (the caller frees it), its size in *zn */
static unsigned char *roundtrip(const unsigned char *dict, size_t dlen, const unsigned char *p, size_t n, size_t *zn)
{
    unsigned char *z, *back;
    *zn = lz_encode(dict, dlen, p, n, &z);
    CHECK(pz_lz_rawsize(z, *zn) == n);
    const size_t r = unpack_guarded(z, *zn, dict, dlen, n, &back);
    CHECK(r == n && (!n || memcmp(back, p, n) == 0));
    free(back);
    return z;
}

static void roundtrip_free(const unsigned char *dict, size_t dlen, const unsigned char *p, size_t n)
{
    size_t zn;
    free(roundtrip(dict, dlen, p, n, &zn));
}

/* ---- streams built by hand: the format as described in src/lz.c ---- */
static void t_known_streams(void)
{
    static const unsigned char dict[11] = "hello world";
    unsigned char *out;
    /* "": size 0, the FNV-1a basis, one empty token */
    static const unsigned char empty[] = {0x00, 0xC5, 0x9D, 0x1C, 0x81, 0x00};
    CHECK(unpack_guarded(empty, sizeof empty, NULL, 0, 0, &out) == 0);
    free(out);
    /* "aaaaaaaa": the literal a, then a match of 7 bytes one back (token: 1 literal, 7 - 4), then the last token */
    unsigned char s1[] = {8, 0, 0, 0, 0, 0x13, 'a', 1, 0x00};
    put32(s1 + 1, fnv((const unsigned char *)"aaaaaaaa", 8));
    CHECK(unpack_guarded(s1, sizeof s1, NULL, 0, 8, &out) == 8 && !memcmp(out, "aaaaaaaa", 8));
    free(out);
    CHECK(unpack_guarded(s1, sizeof s1, NULL, 0, 7, &out) == PZ_LZ_ERR);          /* the text does not fit in the room */
    s1[8] = 0x10;                                                                 /* the last token has a literal the text has no room for */
    CHECK(unpack_guarded(s1, sizeof s1, NULL, 0, 8, &out) == PZ_LZ_ERR);
    s1[8] = 0x01;                                                                 /* the last token has a match length */
    CHECK(unpack_guarded(s1, sizeof s1, NULL, 0, 8, &out) == PZ_LZ_ERR);
    /* 20 literals: the count is 15 + a varint (5) */
    unsigned char s2[7 + 20] = {20, 0, 0, 0, 0, 0xF0, 5};
    for (int k = 0; k < 20; k++) s2[7 + k] = (unsigned char)('A' + k);
    put32(s2 + 1, fnv(s2 + 7, 20));
    CHECK(unpack_guarded(s2, sizeof s2, NULL, 0, 20, &out) == 20 && !memcmp(out, s2 + 7, 20));
    free(out);
    /* a literal, then a match of 31 - 1 = 30 bytes one back: the length is 4 + 15 + a varint (11) */
    unsigned char s3[] = {31, 0, 0, 0, 0, 0x1F, 'a', 1, 11, 0x00}, t3[31];
    memset(t3, 'a', sizeof t3);
    put32(s3 + 1, fnv(t3, sizeof t3));
    CHECK(unpack_guarded(s3, sizeof s3, NULL, 0, 31, &out) == 31 && !memcmp(out, t3, 31));
    free(out);
    /* with a dictionary: "world" is the match at distance 5 into "hello world"; without it, or with another one, the stream is refused */
    unsigned char s4[] = {5, 0, 0, 0, 0, 0x01, 5, 0x00};
    put32(s4 + 1, fnv((const unsigned char *)"world", 5));
    CHECK(unpack_guarded(s4, sizeof s4, dict, 11, 5, &out) == 5 && !memcmp(out, "world", 5));
    free(out);
    CHECK(unpack_guarded(s4, sizeof s4, NULL, 0, 5, &out) == PZ_LZ_ERR);
    CHECK(unpack_guarded(s4, sizeof s4, (const unsigned char *)"hello vorld", 11, 5, &out) == PZ_LZ_ERR);   /* the checksum */
    CHECK(unpack_guarded(s4, sizeof s4, dict, 3, 5, &out) == PZ_LZ_ERR);          /* the match would start before the dictionary */
    /* a two-byte distance: 10 bytes copied from 200 back, which is the start of the dictionary */
    unsigned char big[200], s5[] = {10, 0, 0, 0, 0, 0x06, 0xC8, 0x01, 0x00};
    for (size_t k = 0; k < sizeof big; k++) big[k] = (unsigned char)rnd();
    put32(s5 + 1, fnv(big, 10));
    CHECK(unpack_guarded(s5, sizeof s5, big, sizeof big, 10, &out) == 10 && !memcmp(out, big, 10));
    free(out);
    s5[6] = 0xC9;                                                                 /* 201 back: before the dictionary */
    CHECK(unpack_guarded(s5, sizeof s5, big, sizeof big, 10, &out) == PZ_LZ_ERR);
    s5[6] = 0x80; s5[7] = 0x00;                                                   /* distance 0 */
    CHECK(unpack_guarded(s5, sizeof s5, big, sizeof big, 10, &out) == PZ_LZ_ERR);
    /* a dictionary longer than the buffer */
    unsigned char *small = xrealloc(NULL, 11);
    memset(small, 7, 11);
    CHECK(pz_lz_unpack(s4, sizeof s4, small, 12, 11) == PZ_LZ_ERR);
    CHECK(pz_lz_unpack(s4, sizeof s4, small, 11, 11) == PZ_LZ_ERR);               /* no room for the text */
    free(small);
    /* the room: one byte short, more than enough */
    CHECK(unpack_guarded(s4, sizeof s4, dict, 11, 4, &out) == PZ_LZ_ERR);
    CHECK(unpack_guarded(s4, sizeof s4, dict, 11, 100, &out) == 5 && !memcmp(out, "world", 5));
    free(out);
}

static void t_varints(void)
{
    static const unsigned char ok5[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};            /* 2^32 - 1 */
    static const unsigned char big5[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x10};           /* 2^32 */
    static const unsigned char six[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x00};      /* more than five bytes */
    static const unsigned char cut[] = {0x80, 0x80};
    unsigned char *out;
    CHECK(pz_lz_rawsize(ok5, sizeof ok5) == 0xFFFFFFFFu);
    CHECK(pz_lz_rawsize(big5, sizeof big5) == PZ_LZ_ERR);
    CHECK(pz_lz_rawsize(six, sizeof six) == PZ_LZ_ERR);
    CHECK(pz_lz_rawsize(cut, sizeof cut) == PZ_LZ_ERR);
    CHECK(pz_lz_rawsize(cut, 0) == PZ_LZ_ERR);
    CHECK(unpack_guarded(ok5, sizeof ok5, NULL, 0, 100, &out) == PZ_LZ_ERR);      /* a size that does not fit; a stream that stops */
    CHECK(unpack_guarded(big5, sizeof big5, NULL, 0, 100, &out) == PZ_LZ_ERR);
    CHECK(unpack_guarded(cut, 0, NULL, 0, 100, &out) == PZ_LZ_ERR);
    /* a literal count and a match length of 2^32 - 1 + 15 in a stream of a few bytes */
    unsigned char s[] = {1, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 'a', 0x01};
    put32(s + 1, fnv((const unsigned char *)"a", 1));
    CHECK(unpack_guarded(s, sizeof s, NULL, 0, 1, &out) == PZ_LZ_ERR);
    unsigned char m[] = {8, 0, 0, 0, 0, 0x1F, 'a', 1, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00};
    put32(m + 1, fnv((const unsigned char *)"aaaaaaaa", 8));
    CHECK(unpack_guarded(m, sizeof m, NULL, 0, 8, &out) == PZ_LZ_ERR);

    /* four-byte varints, made by hand: 2^21 + 15 literals, and matches 2^21 - 1, 2^21 and 2^21 + 1 back into a dictionary of that size */
    const size_t lit = 2097152 + 15;
    unsigned char *text = gen_random(lit);
    obuf b = {0};
    ob_varint(&b, lit);
    for (int k = 0; k < 4; k++) ob_put(&b, fnv(text, lit) >> (8 * k) & 255);
    ob_put(&b, 0xF0);
    ob_varint(&b, 2097152);
    for (size_t k = 0; k < lit; k++) ob_put(&b, text[k]);
    CHECK(unpack_guarded(b.p, b.n, NULL, 0, lit, &out) == lit && !memcmp(out, text, lit));
    free(out);
    free(b.p);
    for (size_t dlen = 2097151; dlen <= 2097153; dlen++) {
        obuf s2 = {0};
        ob_varint(&s2, 40);
        for (int k = 0; k < 4; k++) ob_put(&s2, fnv(text, 40) >> (8 * k) & 255);   /* (the dictionary starts like text) */
        ob_put(&s2, 0x0F);                                                        /* no literals, a match of 4 + 15 + 21 bytes */
        ob_varint(&s2, dlen);
        ob_varint(&s2, 21);
        ob_put(&s2, 0x00);
        CHECK(unpack_guarded(s2.p, s2.n, text, dlen, 40, &out) == 40 && !memcmp(out, text, 40));
        free(out);
        CHECK(unpack_guarded(s2.p, s2.n, text, dlen - 1, 40, &out) == PZ_LZ_ERR);   /* one byte too far back */
        free(s2.p);
    }
    free(text);
}

/* ---- the encoder and the decoder on texts of every shape ---- */
static void t_sizes(void)
{
    size_t zn;
    for (size_t n = 0; n <= 300; n++) {
        unsigned char *a = gen_random(n), *b = gen_mixed(n, 256), *c = gen_mixed(n, 3), *d = gen_mixed(n / 2 + 1, 16), *e = xrealloc(NULL, n);
        memset(e, 'x', n);
        unsigned char *z = roundtrip(NULL, 0, a, n, &zn);
        CHECK(zn <= n + 12);                                                       /* random data costs a few bytes of header, no more */
        free(z);
        roundtrip_free(NULL, 0, b, n);
        roundtrip_free(NULL, 0, c, n);
        roundtrip_free(NULL, 0, e, n);
        roundtrip_free(d, n / 2 + 1, a, n);                                        /* a dictionary that has nothing in common with the text */
        roundtrip_free(d, n / 2 + 1, c, n);
        roundtrip_free(e, n, b, n);
        free(a); free(b); free(c); free(d); free(e);
    }
}

/* texts that put the end of every field of the format at a boundary: literal counts and match lengths around 15, 15 + 127, 15 + 128, ...,
 * distances around 127, 128, 16383, 16384, 2^21 - 1, 2^21 */
static void t_boundaries(void)
{
    static const size_t lits[] = {1, 14, 15, 16, 17, 142, 143, 144, 145, 16398, 16399, 16400, 16401};
    static const size_t offs[] = {1, 2, 3, 127, 128, 129, 16383, 16384, 16385};
    static const size_t mlens[] = {4, 5, 6, 18, 19, 20, 21, 33, 34, 146, 147, 148, 149, 16403, 16404, 16405};
    for (size_t i = 0; i < sizeof lits / sizeof *lits; i++)
        for (size_t j = 0; j < sizeof offs / sizeof *offs; j++)
            for (size_t k = 0; k < sizeof mlens / sizeof *mlens; k++) {
                const size_t nl = lits[i] > offs[j] ? lits[i] : offs[j], n = nl + mlens[k] + 7;
                unsigned char *t = gen_random(n);
                for (size_t o = nl; o < nl + mlens[k]; o++) t[o] = t[o - offs[j]];     /* the match (overlapping when the distance is short) */
                roundtrip_free(NULL, 0, t, n);
                free(t);
            }
    /* a single run of literals of every length around the limits of the varint */
    static const size_t runs[] = {14, 15, 16, 142, 143, 144, 16398, 16399, 16400, 16401};
    for (size_t i = 0; i < sizeof runs / sizeof *runs; i++) {
        unsigned char *t = gen_random(runs[i]);
        size_t zn;
        unsigned char *z = roundtrip(NULL, 0, t, runs[i], &zn);
        CHECK(zn <= runs[i] + 16);
        free(z); free(t);
    }
    /* distances of 2^21 - 1, 2^21 and 2^21 + 1 into a dictionary: three-byte and four-byte varints */
    for (size_t dlen = 2097151; dlen <= 2097153; dlen++) {
        unsigned char *d = gen_random(dlen), *t = gen_random(47);
        memcpy(t, d, 40);
        size_t zn;
        unsigned char *z = roundtrip(d, dlen, t, 47, &zn);
        CHECK(zn < 64);                                                           /* the match is used */
        free(z); free(t); free(d);
    }
}

static void t_shapes(void)
{
    size_t zn, sizes[] = {1, 2, 3, 4, 5, 100, 4096, 70000, 1 << 20};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        unsigned char *z0 = xrealloc(NULL, sizes[i]), *per = xrealloc(NULL, sizes[i]);
        memset(z0, 0, sizes[i]);
        for (size_t k = 0; k < sizes[i]; k++) per[k] = "abcdefg"[k % 7];
        unsigned char *z = roundtrip(NULL, 0, z0, sizes[i], &zn);
        CHECK(zn < 40);                                                           /* one literal, one long match at distance 1 */
        free(z);
        z = roundtrip(NULL, 0, per, sizes[i], &zn);
        CHECK(zn < 60);
        free(z);
        free(z0); free(per);
    }
    /* many random texts, all alphabets, with and without a dictionary taken from another text */
    for (int i = 0; i < 400; i++) {
        const size_t n = rnd() % 4 == 0 ? rnd() % 60000 : rnd() % 3000, dn = rnd() % 4000;
        const unsigned alph[] = {2, 4, 16, 256};
        unsigned char *t = gen_mixed(n, alph[rnd() % 4]), *d = gen_mixed(dn, alph[rnd() % 4]);
        roundtrip_free(NULL, 0, t, n);
        roundtrip_free(d, dn, t, n);
        unsigned char *like = clone(d, dn);                                         /* a text that is the dictionary with a few changes */
        for (int k = 0; dn && k < 5; k++) like[rnd() % dn] = (unsigned char)rnd();
        roundtrip_free(d, dn, like, dn);
        free(like); free(t); free(d);
    }
}

/* ---- the real modules ---- */
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static unsigned char *load(const char *dir, const char *name, size_t *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return read_file(path, n);
}

static void t_ptx_modules(void)
{
    DIR *d = opendir(PTX_DIR);
    char *names[128];
    int count = 0;
    CHECK(d != NULL);
    for (struct dirent *e; d && (e = readdir(d)) && count < 128;) {
        const size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".ptx")) names[count++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    CHECK(count >= 19);
    qsort(names, (size_t)count, sizeof *names, by_name);
    size_t solo_lin = 0, with_lin = 0, solo_kang = 0, with_kang = 0;
    unsigned char *lin128 = NULL, *kang64 = NULL;
    size_t nlin = 0, nkang = 0;
    for (int i = 0; i < count; i++) {
        size_t n, zn;
        unsigned char *t = load(PTX_DIR, names[i], &n);
        unsigned char *z = roundtrip(NULL, 0, t, n, &zn);
        CHECK(zn * 3 < n);                                                         /* text like this packs 4 to 5 times */
        CHECK(memchr(t, 0, n) == NULL);                                            /* (the modules are handed to the driver as C strings) */
        free(z);
        if (!strcmp(names[i], "linear128.ptx")) { lin128 = t; nlin = n; solo_lin = zn; }
        else if (!strcmp(names[i], "kang64.ptx")) { kang64 = t; nkang = n; solo_kang = zn; }
        else free(t);
    }
    /* modules of a family against each other: the dictionary makes them cost a few KiB at most */
    for (int i = 0; i < count; i++) {
        size_t n, zn;
        const int lin = !strcmp(names[i], "linear256.ptx") || !strcmp(names[i], "linear512.ptx"), kang = !strcmp(names[i], "kang128.ptx");
        if (!lin && !kang) continue;
        unsigned char *t = load(PTX_DIR, names[i], &n);
        unsigned char *z = roundtrip(lin ? lin128 : kang64, lin ? nlin : nkang, t, n, &zn);
        if (lin) with_lin = zn; else with_kang = zn;
        free(z); free(t);
    }
    CHECK(lin128 && kang64 && with_lin * 20 < solo_lin && with_kang * 2 < solo_kang);
    for (int i = 0; i < count; i++) free(names[i]);
    free(lin128); free(kang64);
}

/* ---- refusals ---- */
static void t_truncated_and_extended(void)
{
    unsigned char *texts[3];
    size_t ns[3] = {1500, 6000, 300};
    texts[0] = gen_mixed(ns[0], 4);
    texts[1] = gen_mixed(ns[1], 256);
    texts[2] = gen_random(ns[2]);
    for (int i = 0; i < 3; i++) {
        size_t zn;
        unsigned char *dict = gen_mixed(500, 4), *z = roundtrip(i == 1 ? dict : NULL, i == 1 ? 500 : 0, texts[i], ns[i], &zn), *out;
        const size_t dl = i == 1 ? 500 : 0;
        for (size_t k = 0; k < zn; k++)                                            /* every proper prefix */
            CHECK(unpack_guarded(z, k, dl ? dict : NULL, dl, ns[i], &out) == PZ_LZ_ERR);
        for (int extra = 1; extra <= 3; extra++) {                                 /* anything after the end */
            unsigned char *longer = xrealloc(NULL, zn + 3);
            memcpy(longer, z, zn);
            for (int k = 0; k < extra; k++) longer[zn + (size_t)k] = (unsigned char)(k ? rnd() : 0);
            CHECK(unpack_guarded(longer, zn + (size_t)extra, dl ? dict : NULL, dl, ns[i], &out) == PZ_LZ_ERR);
            free(longer);
        }
        CHECK(unpack_guarded(z, zn, dl ? dict : NULL, dl, ns[i] - 1, &out) == PZ_LZ_ERR);   /* not enough room */
        free(z); free(dict);
    }
    for (int i = 0; i < 3; i++) free(texts[i]);
}

static void t_corrupted(void)
{
    /* every bit of small streams, then random changes of bigger ones, a real module included */
    for (int i = 0; i < 4; i++) {
        const size_t n = i == 3 ? 40 : 300 + (size_t)i * 100;
        unsigned char *t = i == 2 ? gen_random(n) : gen_mixed(n, i ? 3 : 256), *z;
        size_t zn;
        z = roundtrip(NULL, 0, t, n, &zn);
        for (size_t k = 0; k < zn; k++)
            for (int b = 0; b < 8; b++) {
                z[k] ^= (unsigned char)(1 << b);
                rejected_or_same(z, zn, NULL, 0, t, n);
                z[k] ^= (unsigned char)(1 << b);
            }
        free(z); free(t);
    }
    size_t n, n2, zn, zn2;
    unsigned char *t = load(PTX_DIR, "kang64.ptx", &n), *t2 = load(PTX_DIR, "kang128.ptx", &n2);
    unsigned char *z = roundtrip(NULL, 0, t, n, &zn), *z2 = roundtrip(t, n, t2, n2, &zn2);      /* alone, and against a dictionary */
    for (int i = 0; i < 1500; i++) {
        const int dict = i & 1;
        unsigned char *c = clone(dict ? z2 : z, dict ? zn2 : zn);
        const size_t cn = dict ? zn2 : zn;
        const int ops = 1 + (int)(rnd() % 4);
        for (int k = 0; k < ops; k++) {
            switch (rnd() % 4) {
            case 0: c[rnd() % cn] = (unsigned char)rnd(); break;
            case 1: c[rnd() % cn] ^= (unsigned char)(1u << (rnd() % 8)); break;
            case 2: c[rnd() % cn] = (unsigned char)"\x00\x0f\x10\x1f\x7f\x80\xf0\xff"[rnd() % 8]; break;
            default: c[rnd() % (cn < 64 ? cn : 64)] ^= 0x80; break;                 /* the header and the first sequences */
            }
        }
        rejected_or_same(c, cn, dict ? t : NULL, dict ? n : 0, dict ? t2 : t, dict ? n2 : n);
        free(c);
    }
    free(z); free(z2); free(t); free(t2);
}

static void t_garbage(void)
{
    unsigned char *dict = gen_mixed(40, 4), *out;
    static const unsigned char alphabet[] = {0x00, 0x01, 0x0F, 0x10, 0x1F, 0x13, 0x7F, 0x80, 0x81, 0xF0, 0xFF, 0x04, 0x05};
    for (int i = 0; i < 60000; i++) {
        unsigned char z[96];
        const size_t zn = rnd() % sizeof z, dl = rnd() % 3 ? 0 : rnd() % 41, room = rnd() % 4 ? rnd() % 300 : rnd() % 4;
        for (size_t k = 0; k < zn; k++) z[k] = (rnd() & 1) ? alphabet[rnd() % sizeof alphabet] : (unsigned char)rnd();
        if (zn > 6 && (rnd() & 1)) z[0] = (unsigned char)(rnd() % (room + 1) & 127);       /* a size that fits, so that the sequences are read */
        const size_t r = unpack_guarded(z, zn, dict, dl, room, &out);
        CHECK(r == PZ_LZ_ERR || out != NULL);                                      /* (the checksum makes a success by chance practically impossible) */
        free(out);
    }
    free(dict);
}

/* Streams with a valid checksum made at random, with the text built next to them by a loop of our own: the decoder must produce it. */
static void t_random_streams(void)
{
    for (int i = 0; i < 3000; i++) {
        const size_t dlen = rnd() % 3 ? 0 : rnd() % 700, want = rnd() % 50 ? rnd() % 2500 : rnd() % 90000;
        unsigned char *text = xrealloc(NULL, dlen + want + 1), *dict = dlen ? gen_mixed(dlen, 8) : NULL;
        if (dlen) memcpy(text, dict, dlen);
        obuf s = {0};
        size_t o = dlen;
        const size_t end = dlen + want;
        for (;;) {
            const size_t remain = end - o;
            size_t lit, off = 0, mlen = 0;
            if (remain < (o ? 4u : 5u) || rnd() % 11 == 0) lit = remain;           /* the last sequence */
            else {
                lit = rnd() % 3 ? rnd() % 9 : rnd() % 400;
                if (lit > remain - 4) lit = remain - 4;
                if (o + lit == 0) lit = 1;                                         /* a match needs something to copy */
            }
            const size_t fresh = o;
            for (size_t k = 0; k < lit; k++) text[o++] = (unsigned char)(rnd() % 6);
            if (lit < remain) {
                off = 1 + rnd() % (rnd() % 5 ? (o < 200 ? o : 200) : o);
                mlen = 4 + (rnd() % 6 ? rnd() % 20 : rnd() % 2000);
                if (mlen > end - o) mlen = end - o;
                if (rnd() % 20 == 0) off = o;                                      /* all the way back, into the dictionary when there is one */
            }
            emit_seq(&s, text + fresh, lit, off, mlen);
            for (size_t k = 0; k < mlen; k++, o++) text[o] = text[o - off];
            if (!mlen) break;
        }
        CHECK(o == end);
        unsigned char *z = xrealloc(NULL, s.n + 16), *out;
        obuf hd = {0};
        ob_varint(&hd, want);
        memcpy(z, hd.p, hd.n);
        put32(z + hd.n, fnv(text + dlen, want));
        memcpy(z + hd.n + 4, s.p, s.n);
        const size_t zn = hd.n + 4 + s.n;
        CHECK(unpack_guarded(z, zn, dict, dlen, want, &out) == want && (!want || memcmp(out, text + dlen, want) == 0));
        free(out);
        if (zn > 8) {                                                              /* and the same stream with a mistake in it */
            z[hd.n + 4 + rnd() % (s.n)] ^= (unsigned char)(1u << (rnd() % 8));
            rejected_or_same(z, zn, dict, dlen, text + dlen, want);
        }
        free(z); free(hd.p); free(s.p); free(text); free(dict);
    }
}

/* ---- pz_blob_text ---- */
static void t_blobs(void)
{
    enum { LEVELS = 20 };
    static unsigned char *texts[LEVELS], *zs[LEVELS];
    static size_t ns[LEVELS], zns[LEVELS];
    static pz_blob blobs[LEVELS];
    texts[0] = gen_mixed(30000, 6);
    ns[0] = 30000;
    for (int i = 1; i < LEVELS; i++) {                                             /* every level is the one before it with a few changes */
        ns[i] = ns[i - 1];
        texts[i] = clone(texts[i - 1], ns[i]);
        for (int k = 0; k < 30; k++) texts[i][rnd() % ns[i]] = (unsigned char)('a' + rnd() % 26);
    }
    for (int i = 0; i < LEVELS; i++) {
        zns[i] = lz_encode(i ? texts[i - 1] : NULL, i ? ns[i - 1] : 0, texts[i], ns[i], &zs[i]);
        blobs[i] = (pz_blob){zs[i], zns[i], i ? &blobs[i - 1] : NULL};
    }
    for (int i = 0; i < 3; i++) {
        size_t len = 0;
        char *t = pz_blob_text(&blobs[i], &len);
        CHECK(t && len == ns[i] && memcmp(t, texts[i], len) == 0 && t[len] == 0);
        free(t);
    }
    CHECK(zns[2] < ns[2] / 20);                                                    /* a level costs the few changes, not the text */
    size_t len;
    CHECK(pz_blob_text(&blobs[LEVELS - 1], &len) == NULL);                         /* a chain of bases that is too long */
    pz_blob wrong = {zs[2], zns[2], &blobs[0]};                                    /* not the text it was encoded against */
    CHECK(pz_blob_text(&wrong, &len) == NULL);
    pz_blob alone = {zs[2], zns[2], NULL};
    CHECK(pz_blob_text(&alone, &len) == NULL);                                     /* no base at all */
    pz_blob self = {zs[0], zns[0], &self};                                         /* a cycle */
    CHECK(pz_blob_text(&self, &len) == NULL);
    unsigned char *bad = clone(zs[1], zns[1]);
    bad[zns[1] / 2] ^= 0x40;
    pz_blob corrupt = {bad, zns[1], &blobs[0]}, above = {zs[2], zns[2], &corrupt};
    CHECK(pz_blob_text(&corrupt, &len) == NULL && pz_blob_text(&above, &len) == NULL);   /* an error in a base reaches the modules above it */
    static const unsigned char huge[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0, 0, 0, 0, 0};
    pz_blob toobig = {huge, sizeof huge, NULL};
    CHECK(pz_blob_text(&toobig, &len) == NULL);                                    /* no allocation of 4 GiB for a size in a header */
    pz_blob nothing = {huge, 0, NULL};
    CHECK(pz_blob_text(&nothing, &len) == NULL);
    free(bad);
    for (int i = 0; i < LEVELS; i++) { free(texts[i]); free(zs[i]); }
}

int main(void)
{
    t_known_streams();
    t_varints();
    t_sizes();
    t_boundaries();
    t_shapes();
    t_ptx_modules();
    t_truncated_and_extended();
    t_corrupted();
    t_garbage();
    t_random_streams();
    t_blobs();
    puts(fails ? "t_lz: FAILED" : "t_lz: ok");
    return fails != 0;
}
