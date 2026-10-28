/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_kernels.c - the PTX modules embedded in the program (compressed, build/ptx_gen.c) against the files of src/kernels/ptx: every module
 * must unpack to exactly the bytes of its file, with the NUL that ends the text, which is how the CUDA driver (real or simulated) is
 * given it. The simulated driver of tests/mock_cuda recognises a module by comparing the text it is handed with its own copy of the
 * file (embedded plain, by tools/embed.c without -z): this test is the same comparison, made on its own and for every module, and it
 * also fails if a file in src/kernels/ptx is not embedded. Run it from the root of the repository. */
#include "../src/lz.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PTX_DIR
#define PTX_DIR "src/kernels/ptx"
#endif

extern const int pz_embedded_count;
extern const char *const pz_embedded_names[], *const pz_embedded_files[];
extern const pz_blob *const pz_embedded_blobs[];

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static unsigned char *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0, got;
    unsigned char *b = malloc(cap);
    while (b && (got = fread(b + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
    }
    fclose(f);
    *n = len;
    return b;
}

int main(void)
{
    CHECK(pz_embedded_count >= 19);
    size_t total = 0;
    clock_t t0 = clock();
    for (int i = 0; i < pz_embedded_count; i++) {
        size_t fn = 0, tn = 0;
        unsigned char *file = read_all(pz_embedded_files[i], &fn);
        char *text = pz_blob_text(pz_embedded_blobs[i], &tn);
        if (!file || !text) { printf("FAIL %s: %s\n", pz_embedded_names[i], !file ? "cannot read the file" : "cannot unpack the module"); fails++; }
        else {
            const int same = tn == fn && memcmp(text, file, fn) == 0;
            if (!same) printf("FAIL %s: the unpacked module differs from %s\n", pz_embedded_names[i], pz_embedded_files[i]);
            CHECK(same);
            CHECK(text[tn] == 0 && strlen(text) == tn);               /* a C string with the same length as the file (no NUL inside) */
            total += tn;
        }
        free(file);
        free(text);
        for (int j = 0; j < i; j++) CHECK(strcmp(pz_embedded_files[i], pz_embedded_files[j]) && strcmp(pz_embedded_names[i], pz_embedded_names[j]));
    }
    const double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
    /* every file of the folder is embedded */
    DIR *d = opendir(PTX_DIR);
    CHECK(d != NULL);
    int files = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        const size_t l = strlen(e->d_name);
        if (l <= 4 || strcmp(e->d_name + l - 4, ".ptx")) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", PTX_DIR, e->d_name);
        int found = 0;
        for (int i = 0; i < pz_embedded_count; i++) found |= strcmp(path, pz_embedded_files[i]) == 0;
        if (!found) printf("FAIL %s is not embedded (add it to PTXMODS in the Makefile)\n", path);
        CHECK(found);
        files++;
    }
    if (d) closedir(d);
    CHECK(files == pz_embedded_count);
    printf("t_kernels: %s (%d modules, %.1f MiB unpacked in %.0f ms)\n", fails ? "FAILED" : "ok", pz_embedded_count, (double)total / 1048576.0, ms);
    return fails != 0;
}
