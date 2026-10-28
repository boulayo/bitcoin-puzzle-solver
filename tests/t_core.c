/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_core.c - the platform layer's file helpers. (The primitives - field, curve, hashes, addresses, permutation - are tested in
 * lib/pzcore/tests, which link nothing but the library.) */
#include "../src/platform.h"
#include <pzcore/ec.h>
#include <pzcore/hash.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dirent.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- several writers of one file (the caches are shared by every pzsolve of the user) ---- */

#define WR_SIZE 3000
#define WR_ROUNDS 150

static const char *g_wr_path;
static _Atomic int g_wr_bad, g_wr_stop;

/* Every write is WR_SIZE copies of one letter: a torn file mixes letters, a short one has the wrong length. */
static int writer_loop(int id)
{
    char buf[WR_SIZE];
    int bad = 0;
    for (int i = 0; i < WR_ROUNDS; i++) {
        memset(buf, 'a' + (id * 7 + i) % 26, sizeof buf);
        if (pz_write_file_atomic(g_wr_path, buf, sizeof buf, 0644) != 0) bad++;
    }
    return bad;
}

static void *writer_thread(void *p)
{
    atomic_fetch_add(&g_wr_bad, writer_loop((int)(intptr_t)p));
    return NULL;
}

static void *reader_thread(void *p)
{
    (void)p;
    while (!atomic_load(&g_wr_stop)) {
        size_t n = 0;
        char *t = pz_read_file(g_wr_path, &n);
        if (!t) continue;                                             /* before the first write */
        bool uniform = n == WR_SIZE;
        for (size_t i = 1; i < n && uniform; i++) uniform = t[i] == t[0];
        if (!uniform) atomic_fetch_add(&g_wr_bad, 1);
        free(t);
    }
    return NULL;
}

/* One line of the cache file per writer: the lines of the others must survive. */
static int keyed_loop(const char *path, int id)
{
    char key[32], val[32];
    int bad = 0;
    snprintf(key, sizeof key, "writer%d", id);
    for (int i = 0; i < 40; i++) {
        snprintf(val, sizeof val, "round=%d", i);
        if (pz_keyed_file_put(path, key, val) != 0) bad++;
    }
    return bad;
}

static void t_writers(void)
{
    char dir[600], path[700], tmpn[800];
    if (!pz_make_temp_dir(dir, sizeof dir, "pzwr")) { CHECK(0); return; }
    snprintf(path, sizeof path, "%s/shared.txt", dir);
    g_wr_path = path;
    /* threads of one process: the temporary names differ by the counter */
    pthread_t w[3], rd;
    pthread_create(&rd, NULL, reader_thread, NULL);
    for (int i = 0; i < 3; i++) pthread_create(&w[i], NULL, writer_thread, (void *)(intptr_t)i);
    for (int i = 0; i < 3; i++) pthread_join(w[i], NULL);
    atomic_store(&g_wr_stop, 1);
    pthread_join(rd, NULL);
    CHECK(atomic_load(&g_wr_bad) == 0);
#ifndef _WIN32
    /* processes: the temporary names differ by the process id */
    pid_t kids[4];
    fflush(stdout);
    for (int i = 0; i < 3; i++) {
        kids[i] = fork();
        if (kids[i] == 0) _exit(writer_loop(10 + i) == 0 ? 0 : 1);
    }
    int failed = 0;
    for (int i = 0; i < 3; i++) {
        int st = 0;
        if (kids[i] < 0 || waitpid(kids[i], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) failed++;
    }
    CHECK(failed == 0);
    size_t n = 0;
    char *t = pz_read_file(path, &n);
    CHECK(t && n == WR_SIZE);
    free(t);
    /* nothing but the file and its lock is left in the folder */
    DIR *d = opendir(dir);
    int extra = 0;
    for (struct dirent *e; d && (e = readdir(d));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "shared.txt")) extra++;
    if (d) closedir(d);
    CHECK(extra == 0);
    /* keyed lines: no program loses the line of another */
    snprintf(tmpn, sizeof tmpn, "%s/keyed.txt", dir);
    for (int i = 0; i < 4; i++) {
        kids[i] = fork();
        if (kids[i] == 0) _exit(keyed_loop(tmpn, i) == 0 ? 0 : 1);
    }
    for (int i = 0; i < 4; i++) waitpid(kids[i], NULL, 0);
    t = pz_read_file(tmpn, NULL);
    CHECK(t != NULL);
    for (int i = 0; t && i < 4; i++) {
        char want[48];
        snprintf(want, sizeof want, "writer%d=round=39\n", i);
        CHECK(strstr(t, want) != NULL);
    }
    free(t);
#else
    (void)tmpn;
    CHECK(keyed_loop(path, 0) == 0);
#endif
    pz_remove_tree(dir);
}

int main(void)
{
    {   /* the library is linked and gives the well-known answer for key 1 */
        pz_u256 k = pz_u256_from_u64(1);
        pz_ge p;
        uint8_t pub[33], h[20];
        char hx[41];
        CHECK(pz_ec_mul_g(&p, &k));
        pz_ge_to_pub33(pub, &p);
        pz_hash160(pub, 33, h);
        pz_hex_encode(h, 20, hx);
        CHECK(!strcmp(hx, "751e76e8199196d454941c45d1b3a323f1433bd6"));
    }
#ifndef _WIN32
    {   /* a file that holds a secret is created private (FOUND.txt carries the private key); an ordinary one stays readable */
        char dir[600], path[700];
        if (pz_make_temp_dir(dir, sizeof dir, "pzperm")) {
            struct stat st;
            snprintf(path, sizeof path, "%s/secret", dir);
            CHECK(pz_write_file_atomic(path, "k", 1, 0600) == 0);
            CHECK(stat(path, &st) == 0 && (st.st_mode & 077) == 0);
            snprintf(path, sizeof path, "%s/ordinary", dir);
            CHECK(pz_write_file_atomic(path, "k", 1, 0644) == 0);
            CHECK(stat(path, &st) == 0 && (st.st_mode & 0600) == 0600);
            pz_remove_tree(dir);
        }
    }
#endif
    t_writers();
    {   /* the log is appended in binary mode: lines end in LF on Windows too */
        char dir[600], path[700];
        if (pz_make_temp_dir(dir, sizeof dir, "pzlog")) {
            snprintf(path, sizeof path, "%s/log.txt", dir);
            pz_log_set_quiet(1);
            pz_log_set_file(path);
            pz_log(PZ_LOG_INFO, "first line");
            pz_log_set_file(NULL);
            pz_log_set_file(path);                                   /* reopened for append: the two lines are kept */
            pz_log(PZ_LOG_INFO, "second line");
            pz_log_set_file(NULL);
            pz_log_set_quiet(0);
            char *t = pz_read_file(path, NULL);
            CHECK(t && strstr(t, "first line") && strstr(t, "second line") && !strchr(t, '\r'));
            int lines = 0;
            for (const char *q = t ? t : ""; *q; q++) lines += *q == '\n';
            CHECK(lines == 2);
            free(t);
            pz_remove_tree(dir);
        }
    }
    printf(fails ? "t_core: %d FAILED\n" : "t_core: OK\n", fails);
    return fails != 0;
}
