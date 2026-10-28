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

#ifndef _WIN32
/* Messages and dashboard redraws from another thread while the main thread dies. */
static void *noisy_thread(void *p)
{
    (void)p;
    for (int i = 0; i < 200000; i++) {
        pz_log(PZ_LOG_INFO, "noise from another thread");
        pz_ui_block("block", 1);
    }
    return NULL;
}

/* pz_die writes its message to the campaign log (under the lock of the log, as the other threads do), then exits with status 2. */
static void t_die_log(void)
{
    char dir[600], path[700];
    if (!pz_make_temp_dir(dir, sizeof dir, "pzdie")) return;
    snprintf(path, sizeof path, "%s/log.txt", dir);
    fflush(NULL);                                                   /* the child would write what is still buffered a second time */
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60);                                                  /* a deadlock ends the child by a signal, not by exit(2) */
        if (!freopen("/dev/null", "w", stderr)) _exit(99);
        pz_log_set_quiet(2);
        pz_log_set_file(path);
        pthread_t th;
        pthread_create(&th, NULL, noisy_thread, NULL);
        pthread_detach(th);                                         /* nobody joins it: the process ends in pz_die (no "thread leak" under TSan) */
        pz_log(PZ_LOG_INFO, "before");
        pz_die("fatal %s, %d", "reason", 7);
    }
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2);
    char *t = pz_read_file(path, NULL);
    CHECK(t != NULL);
    int errors = 0, odd = 0;
    for (char *l = t; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        const char *e = strstr(l, "[ERROR] fatal reason, 7");
        if (e && !e[sizeof "[ERROR] fatal reason, 7" - 1]) errors++;
        else if (!strstr(l, "[INFO] noise from another thread") && !strstr(l, "[INFO] before")) odd++;
        l = nl ? nl + 1 : NULL;
    }
    CHECK(errors == 1 && odd == 0);                                 /* the message once, and no line torn by the other thread */
    free(t);
    pz_remove_tree(dir);
}
#endif

/* pz_file_count reads in blocks of 64 KiB: a match across the end of a block is found once, and none is lost or counted twice. */
static void t_file_count(void)
{
    char dir[600], path[700];
    if (!pz_make_temp_dir(dir, sizeof dir, "pzcnt")) return;
    snprintf(path, sizeof path, "%s/log.txt", dir);
    const char *nd = "system suspension detected";
    const size_t nl = strlen(nd), total = 200000;
    char *buf = pz_xmalloc(total);
    CHECK(pz_file_count(path, nd) == -1);                                     /* no such file */
    int bad = 0, tried = 0;
    for (size_t k = 0; k <= 3; k++)
        for (long d = -(long)nl - 2; d <= 2; d++) {
            const long pos = (long)(65536 * k) + d;
            if (pos < 0 || (size_t)pos + nl > total) continue;
            memset(buf, 'x', total);
            buf[3] = 0;                                                       /* a NUL in the text does not end the search */
            memcpy(buf + pos, nd, nl);
            CHECK(pz_write_file_atomic(path, buf, total, 0644) == 0);
            tried++;
            if (pz_file_count(path, nd) != 1) bad++;
        }
    memset(buf, 'x', total);
    memcpy(buf + total - nl, nd, nl);                                         /* at the very end */
    memcpy(buf, nd, nl);                                                      /* and at the very start */
    memcpy(buf + 65536 - 3, nd, nl);                                          /* and across the first block boundary */
    CHECK(pz_write_file_atomic(path, buf, total, 0644) == 0);
    CHECK(pz_file_count(path, nd) == 3);
    CHECK(tried > 90 && bad == 0);
    memset(buf, 'a', 70000);
    CHECK(pz_write_file_atomic(path, buf, 70000, 0644) == 0);
    CHECK(pz_file_count(path, "aaaa") == 70000 - 3);                          /* overlapping matches all count, also across a boundary */
    CHECK(pz_file_count(path, "a") == 70000);
    CHECK(pz_file_count(path, "b") == 0);
    CHECK(pz_write_file_atomic(path, "", 0, 0644) == 0);
    CHECK(pz_file_count(path, nd) == 0);                                      /* empty file */
    CHECK(pz_write_file_atomic(path, "ab", 2, 0644) == 0);
    CHECK(pz_file_count(path, "abc") == 0);                                   /* shorter than the needle */
    free(buf);
    pz_remove_tree(dir);
}

/* pz_kv_find: the value of "key<sep>value" in a text (the readers of campaign.conf, the checkpoint, status.json, /proc/cpuinfo, /proc/meminfo,
 * session.txt and the machine profile all use it). */
static void t_kv(void)
{
    const char *v;
    CHECK(pz_kv_find("a=1\nb=2\n", "a", '=', 0) && !strcmp(pz_kv_find("a=1\nb=2\n", "a", '=', 0), "1\nb=2\n"));
    CHECK(pz_kv_find("a=1\nb=2\n", "b", '=', 0) && !strcmp(pz_kv_find("a=1\nb=2\n", "b", '=', 0), "2\n"));
    CHECK(pz_kv_find("a=1\nb=2\n", "c", '=', 0) == NULL);
    CHECK(pz_kv_find("", "a", '=', 0) == NULL);
    /* the first match by default, the last with PZ_KV_LAST */
    const char *dup = "k=1\nx=0\nk=2\nk=3";
    CHECK((v = pz_kv_find(dup, "k", '=', 0)) && !strncmp(v, "1\n", 2));
    CHECK((v = pz_kv_find(dup, "k", '=', PZ_KV_LAST)) && !strcmp(v, "3"));
    /* the key stands at the start of a line, unless PZ_KV_ANYWHERE */
    const char *mid = "  k=1\nxk=2\nk=3\n";
    CHECK((v = pz_kv_find(mid, "k", '=', 0)) && !strncmp(v, "3\n", 2));
    CHECK((v = pz_kv_find(mid, "k", '=', PZ_KV_ANYWHERE)) && !strncmp(v, "1\n", 2));
    CHECK((v = pz_kv_find(mid, "k", '=', PZ_KV_ANYWHERE | PZ_KV_LAST)) && !strncmp(v, "3\n", 2));
    CHECK(pz_kv_find("  k=1\n", "k", '=', 0) == NULL);
    /* a key that is the beginning of a longer name, or a longer key, does not match; the separator comes right after the key */
    CHECK((v = pz_kv_find("cbitsx=1\ncbits=2\n", "cbits", '=', 0)) && !strncmp(v, "2\n", 2));
    CHECK(pz_kv_find("cbitsx=1\n", "cbits", '=', 0) == NULL);
    CHECK(pz_kv_find("cbits\n=5\n", "cbits", '=', 0) == NULL);
    CHECK(pz_kv_find("cbits 5\n", "cbits", '=', 0) == NULL);
    CHECK(pz_kv_find("cbits = 5\n", "cbits", '=', 0) == NULL);
    CHECK(pz_kv_find("cb=1\n", "cbits", '=', 0) == NULL);
    /* nothing after the separator is skipped or cut: blanks, '=' and '\r' are part of the value, the end of the value is the caller's business */
    CHECK((v = pz_kv_find("k= v=w\r\nz=1\n", "k", '=', 0)) && !strncmp(v, " v=w\r\n", 6));
    CHECK((v = pz_kv_find("k=", "k", '=', 0)) && !*v);                              /* an empty value is a match */
    CHECK((v = pz_kv_find("k=\nk=2\n", "k", '=', 0)) && *v == '\n');
    CHECK(pz_kv_find("k", "k", '=', 0) == NULL);
    /* PZ_KV_PAD: blanks and tabs between the key and the separator */
    const char *ci = "processor\t: 0\nmodel name\t: Intel(R) Xeon\nModel name  :  x\nMemTotal:       16384 kB\n";
    CHECK((v = pz_kv_find(ci, "model name", ':', PZ_KV_PAD)) && !strncmp(v, " Intel", 6));
    CHECK(pz_kv_find(ci, "model name", ':', 0) == NULL);
    CHECK((v = pz_kv_find(ci, "Model name", ':', PZ_KV_PAD)) && !strncmp(v, "  x", 3));
    CHECK((v = pz_kv_find(ci, "MemTotal", ':', 0)) && pz_strtou64(v) == 16384);
    CHECK((v = pz_kv_find(ci, "MemTotal", ':', PZ_KV_ANYWHERE)) && pz_strtou64(v) == 16384);
    CHECK(pz_kv_find(ci, "Total", ':', 0) == NULL && pz_kv_find(ci, "Total", ':', PZ_KV_ANYWHERE) != NULL);
    CHECK(pz_kv_find(ci, "model", ':', PZ_KV_PAD) == NULL);                              /* "model name" is not "model" */
    /* a quoted key, as in status.json */
    const char *js = "{\n  \"state\": \"RUNNING\",\n  \"fraction\": 0.5\n}\n";
    CHECK(pz_kv_find(js, "\"state\"", ':', 0) == NULL);                                  /* indented: not at the start of a line */
    CHECK((v = pz_kv_find(js, "\"state\"", ':', PZ_KV_ANYWHERE)) && !strncmp(v, " \"RUNNING\"", 10));
    CHECK((v = pz_kv_find(js, "\"fraction\"", ':', PZ_KV_ANYWHERE)) && !strncmp(v, " 0.5\n", 5));
    /* the text is a C string: a NUL ends it */
    const char nul[] = "a=1\n\0b=2\n";
    CHECK(pz_kv_find(nul, "b", '=', 0) == NULL && pz_kv_find(nul, "a", '=', 0) != NULL);
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
    t_kv();
    t_file_count();
#ifndef _WIN32
    t_die_log();
#endif
    {   /* the files made again at the next write (report.txt, status.json) are replaced atomically without the wait for the disk */
        char dir[600], path[700];
        if (pz_make_temp_dir(dir, sizeof dir, "pzdur")) {
            snprintf(path, sizeof path, "%s/derived.txt", dir);
            CHECK(pz_write_file_atomic_ex(path, "one", 3, 0644, false) == 0);
            CHECK(pz_write_file_atomic_ex(path, "second", 6, 0644, false) == 0);
            size_t n = 0;
            char *t = pz_read_file(path, &n);
            CHECK(t && n == 6 && !memcmp(t, "second", 6));
            free(t);
            CHECK(pz_write_file_atomic_ex(path, "durable", 7, 0644, true) == 0);
            t = pz_read_file(path, &n);
            CHECK(t && n == 7 && !memcmp(t, "durable", 7));
            free(t);
            DIR *d = opendir(dir);
            int extra = 0;
            for (struct dirent *e; d && (e = readdir(d));)
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "derived.txt")) extra++;
            if (d) closedir(d);
            CHECK(extra == 0);
            pz_remove_tree(dir);
        }
    }
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
