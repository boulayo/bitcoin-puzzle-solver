/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* embed.c - turns text files into C byte arrays: embed OUT.c name1 file1 [name2 file2 ...]
 * Produces: const unsigned char <name>[] = {...,0}; const unsigned int <name>_len = N;
 *
 * It also holds the encoder of the compressed format that src/lz.c decodes; tests/t_lz.c includes this file with EMBED_NO_MAIN set
 * to test it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LZ_MINMATCH 4
#define LZ_HASHBITS 17
#define LZ_DEPTH 32       /* candidates examined per position */
#define LZ_NICE 256       /* a match this long ends the search for a longer one */

typedef struct { unsigned char *p; size_t n, cap; } obuf;

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "embed: out of memory\n"); exit(1); }
    return p;
}

static void ob_put(obuf *o, unsigned c)
{
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 4096; o->p = xrealloc(o->p, o->cap); }
    o->p[o->n++] = (unsigned char)c;
}

static void ob_varint(obuf *o, size_t v)
{
    for (; v >= 128; v >>= 7) ob_put(o, (unsigned)(v & 127) | 128);
    ob_put(o, (unsigned)v);
}

static size_t varint_len(size_t v) { size_t n = 1; for (; v >= 128; v >>= 7) n++; return n; }

/* one sequence: the literals lit[0..nlit), then the match (off, mlen); mlen 0 for the last sequence of the stream */
static void emit_seq(obuf *o, const unsigned char *lit, size_t nlit, size_t off, size_t mlen)
{
    const size_t m = mlen ? mlen - LZ_MINMATCH : 0;
    ob_put(o, (unsigned)((nlit < 15 ? nlit : 15) << 4 | (m < 15 ? m : 15)));
    if (nlit >= 15) ob_varint(o, nlit - 15);
    for (size_t k = 0; k < nlit; k++) ob_put(o, lit[k]);
    if (!mlen) return;
    ob_varint(o, off);
    if (m >= 15) ob_varint(o, m - 15);
}

typedef struct { const unsigned char *b; size_t total; int32_t *head, *prev; size_t next; } matcher;

static uint32_t hash4(const unsigned char *p)
{
    const uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return (v * 2654435761u) >> (32 - LZ_HASHBITS);
}

/* puts the positions before `upto` in the hash chains (the newest first) */
static void mf_insert(matcher *m, size_t upto)
{
    for (; m->next < upto; m->next++) {
        if (m->next + LZ_MINMATCH > m->total) continue;
        const uint32_t h = hash4(m->b + m->next);
        m->prev[m->next] = m->head[h];
        m->head[h] = (int32_t)m->next;
    }
}

/* a match of len bytes at distance off is worth using when it is shorter than what it replaces: the offset takes 1 to 5 bytes */
static int usable(size_t len, size_t off) { return len >= LZ_MINMATCH && len >= 3 + varint_len(off); }
static long score(size_t len, size_t off) { return (long)len - (long)varint_len(off); }

static size_t mf_find(matcher *m, size_t pos, size_t *boff)
{
    size_t best = 0, bestoff = 0;
    mf_insert(m, pos);
    if (pos + LZ_MINMATCH > m->total) return 0;
    const size_t max = m->total - pos;
    int32_t c = m->head[hash4(m->b + pos)];
    for (int depth = 0; c >= 0 && depth < LZ_DEPTH; depth++, c = m->prev[c]) {
        size_t l = 0;
        while (l < max && m->b[(size_t)c + l] == m->b[pos + l]) l++;
        if (!usable(l, pos - (size_t)c)) continue;
        if (!best || score(l, pos - (size_t)c) > score(best, bestoff)) { best = l; bestoff = pos - (size_t)c; }
        if (best >= LZ_NICE) break;
    }
    *boff = bestoff;
    return best;
}

/* Encodes src[0..n) against the dictionary dict[0..dlen) (the matches may point into it) into a new buffer; returns its size.
 * Greedy with one step of lazy matching over hash chains: slow compared with LZ4, but the whole set of PTX modules takes a few seconds. */
static size_t lz_encode(const unsigned char *dict, size_t dlen, const unsigned char *src, size_t n, unsigned char **out)
{
    const size_t total = dlen + n;
    if (total >= (size_t)1 << 31) { fprintf(stderr, "embed: input too big\n"); exit(1); }
    unsigned char *b = xrealloc(NULL, total);
    if (dlen) memcpy(b, dict, dlen);
    if (n) memcpy(b + dlen, src, n);
    matcher m = {b, total, xrealloc(NULL, sizeof(int32_t) << LZ_HASHBITS), xrealloc(NULL, sizeof(int32_t) * (total ? total : 1)), 0};
    memset(m.head, 0xff, sizeof(int32_t) << LZ_HASHBITS);

    obuf o = {0};
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < n; k++) h = (h ^ src[k]) * 16777619u;
    ob_varint(&o, n);
    for (int k = 0; k < 4; k++) ob_put(&o, h >> (8 * k) & 255);

    size_t pos = dlen, lit = dlen;
    while (pos < total) {
        size_t off, len = mf_find(&m, pos, &off);
        if (!len) { pos++; continue; }
        while (len < LZ_NICE && pos + 1 < total) {                       /* a better match one byte on? then this byte is a literal */
            size_t off2, len2 = mf_find(&m, pos + 1, &off2);
            if (!len2 || score(len2, off2) <= score(len, off) + 1) break;
            pos++; len = len2; off = off2;
        }
        emit_seq(&o, b + lit, pos - lit, off, len);
        pos += len;
        lit = pos;
    }
    emit_seq(&o, b + lit, total - lit, 0, 0);
    free(m.head); free(m.prev); free(b);
    *out = o.p;
    return o.n;
}

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *in = fopen(path, "rb");
    if (!in) { perror(path); exit(1); }
    unsigned char *b = NULL;
    size_t len = 0, cap = 0, got;
    do {
        if (len == cap) { cap = cap ? cap * 2 : 65536; b = xrealloc(b, cap); }
        got = fread(b + len, 1, cap - len, in);
        len += got;
    } while (got);
    if (ferror(in)) { perror(path); exit(1); }
    fclose(in);
    *n = len;
    return b;
}

#ifndef EMBED_NO_MAIN

int main(int argc, char **argv)
{
    if (argc < 4 || argc % 2) { fprintf(stderr, "usage: embed OUT.c name file [name file...]\n"); return 2; }
    FILE *out = fopen(argv[1], "w");
    if (!out) { perror(argv[1]); return 1; }
    fprintf(out, "/* generated by tools/embed.c: do not edit */\n");
    for (int a = 2; a < argc; a += 2) {
        FILE *in = fopen(argv[a + 1], "rb");
        if (!in) { perror(argv[a + 1]); return 1; }
        fprintf(out, "const unsigned char %s[] = {\n", argv[a]);
        unsigned long n = 0;
        int c;
        while ((c = fgetc(in)) != EOF) {
            fprintf(out, "%d,", c);
            if (++n % 24 == 0) fputc('\n', out);
        }
        fprintf(out, "0};\nconst unsigned int %s_len = %lu;\n", argv[a], n);
        fclose(in);
    }
    fclose(out);
    return 0;
}

#endif
