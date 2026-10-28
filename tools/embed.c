/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* embed.c - turns files into C arrays, at build time:
 *
 *   embed OUT.c name file [name file...]       plain: const unsigned char <name>[] = {...,0}; const unsigned int <name>_len = N;
 *   embed -z OUT.c name[:base] file [...]      compressed: const pz_blob <name> = {...}  (src/lz.h; the files are the PTX modules)
 *   embed -T OUT.c name[:base] file [...]      only a table of what -z embeds, for the tests: pz_embedded_count, pz_embedded_names[],
 *                                              pz_embedded_files[] (the paths) and pz_embedded_blobs[]
 *
 * With -z every file is compressed with the encoder below (the format is in src/lz.c) and checked at once with the decoder: the build
 * stops if a stream does not unpack to the file. ":base" names an earlier item whose text is used as the dictionary of this one: the
 * PTX modules of a family differ in few lines, so a module stored against its relative takes a few KiB instead of a few hundred; the
 * program unpacks the base first when it loads the module. Build: cc -Isrc tools/embed.c src/lz.c (tests/t_lz.c includes this file
 * with EMBED_NO_MAIN set to test the encoder). */
#include "lz.h"
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

static void put_bytes(FILE *out, const unsigned char *p, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        fprintf(out, "%d,", p[k]);
        if ((k + 1) % 24 == 0) fputc('\n', out);
    }
}

typedef struct { const char *name, *file; int base; unsigned char *raw; size_t n; } item;

int main(int argc, char **argv)
{
    int mode = 0, a = 1;                                  /* 0 plain, 'z' compressed, 'T' table */
    if (a < argc && (!strcmp(argv[a], "-z") || !strcmp(argv[a], "-T"))) mode = argv[a++][1];
    if (argc - a < 3 || (argc - a) % 2 == 0) { fprintf(stderr, "usage: embed [-z | -T] OUT.c name[:base] file [name[:base] file...]\n"); return 2; }
    const char *outpath = argv[a++];
    const int count = (argc - a) / 2;
    item *it = xrealloc(NULL, sizeof *it * (size_t)count);
    for (int k = 0; k < count; k++, a += 2) {
        char *name = argv[a], *colon = strchr(name, ':');
        it[k] = (item){name, argv[a + 1], -1, NULL, 0};
        if (colon) {
            *colon = 0;
            if (!mode) { fprintf(stderr, "embed: %s: a base needs -z\n", argv[a]); return 2; }
            for (int j = 0; j < k; j++) if (!strcmp(it[j].name, colon + 1)) it[k].base = j;
            if (it[k].base < 0) { fprintf(stderr, "embed: %s: the base %s is not an earlier item\n", name, colon + 1); return 2; }
        }
    }
    for (int k = 0; k < count && mode != 'T'; k++) it[k].raw = read_file(it[k].file, &it[k].n);
    FILE *out = fopen(outpath, "w");
    if (!out) { perror(outpath); return 1; }
    fprintf(out, "/* generated by tools/embed.c: do not edit */\n");
    if (mode) fprintf(out, "#include \"lz.h\"\n");
    if (mode == 'T') {
        fprintf(out, "extern const pz_blob");
        for (int k = 0; k < count; k++) fprintf(out, "%s %s", k ? "," : "", it[k].name);
        fprintf(out, ";\nconst int pz_embedded_count = %d;\nconst char *const pz_embedded_names[] = {", count);
        for (int k = 0; k < count; k++) fprintf(out, "\"%s\",", it[k].name);
        fprintf(out, "};\nconst char *const pz_embedded_files[] = {");
        for (int k = 0; k < count; k++) fprintf(out, "\"%s\",", it[k].file);
        fprintf(out, "};\nconst pz_blob *const pz_embedded_blobs[] = {");
        for (int k = 0; k < count; k++) fprintf(out, "&%s,", it[k].name);
        fprintf(out, "};\n");
    }
    for (int k = 0; k < count && mode != 'T'; k++) {
        if (!mode) {
            fprintf(out, "const unsigned char %s[] = {\n", it[k].name);
            put_bytes(out, it[k].raw, it[k].n);
            fprintf(out, "0};\nconst unsigned int %s_len = %lu;\n", it[k].name, (unsigned long)it[k].n);
            continue;
        }
        const item *base = it[k].base < 0 ? NULL : &it[it[k].base];
        const size_t dlen = base ? base->n : 0;
        unsigned char *z, *chk = xrealloc(NULL, dlen + it[k].n);
        const size_t zn = lz_encode(base ? base->raw : NULL, dlen, it[k].raw, it[k].n, &z);
        if (dlen) memcpy(chk, base->raw, dlen);
        if (pz_lz_unpack(z, zn, chk, dlen, dlen + it[k].n) != it[k].n || memcmp(chk + dlen, it[k].raw, it[k].n)) {
            fprintf(stderr, "embed: %s: the compressed stream does not unpack to the file\n", it[k].file);
            fclose(out);
            remove(outpath);                                  /* (an unfinished file must not look up to date to make) */
            return 1;
        }
        fprintf(out, "static const unsigned char %s_z[] = {\n", it[k].name);
        put_bytes(out, z, zn);
        fprintf(out, "};\n");
        fprintf(out, "const pz_blob %s = {%s_z, %lu, %s%s};\n", it[k].name, it[k].name, (unsigned long)zn, base ? "&" : "", base ? base->name : "NULL");
        free(z);
        free(chk);
    }
    if (fclose(out)) { perror(outpath); remove(outpath); return 1; }
    return 0;
}

#endif
