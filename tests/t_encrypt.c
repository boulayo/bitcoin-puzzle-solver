/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_encrypt.c - what pzsolve adds to pzcore's seal: the public key of the options, the associated data, the line of an encrypted key, the field of
 * campaign.conf (and the files without it), the secret read from standard input, the command of --on-found, the hardening of the process. */
#include "../src/encrypt.h"
#include "../src/campaign.h"
#include "../src/platform.h"
#include "../src/onfound.h"
#include "../src/envopts.h"
#include <pzcore/hash.h>
#include <errno.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char g_dir[700];

static void write_text(const char *name, const char *text)
{
    char p[800];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    CHECK(pz_write_file_atomic(p, text, strlen(text), 0600) == 0);
}

static char *read_text(const char *name)
{
    char p[800];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    return pz_read_file(p, NULL);
}

static void test_pubkey(void)
{
    pz_ge q, q2;
    u8 q33[33], q33b[33];
    char err[200];
    const char *comp = "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
    const char *unc = "0479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8";
    CHECK(enc_parse_pubkey(comp, &q, q33, err, sizeof err) && pz_ge_equal(&q, &pz_ec_g));
    CHECK(enc_parse_pubkey(unc, &q2, q33b, err, sizeof err) && pz_ge_equal(&q2, &pz_ec_g) && !memcmp(q33, q33b, 33));
    char id[9];
    enc_key_id(&q, id);
    CHECK(!strcmp(id, "751e76e8"));                                              /* HASH160 of the key 1 starts 751e76e8 */
    CHECK(!enc_parse_pubkey("", &q, q33, err, sizeof err) && err[0]);
    CHECK(!enc_parse_pubkey("0279be", &q, q33, err, sizeof err));
    CHECK(!enc_parse_pubkey("02zzbe667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", &q, q33, err, sizeof err));
    CHECK(!enc_parse_pubkey("0579be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", &q, q33, err, sizeof err));          /* bad prefix */
    CHECK(!enc_parse_pubkey("0600000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", &q, q33, err, sizeof err));
    CHECK(!enc_parse_pubkey("0200000000000000000000000000000000000000000000000000000000000000fe", &q, q33, err, sizeof err));          /* x = 254: not on the curve */
    char bad[131];
    memcpy(bad, unc, 131);
    bad[130 - 1] = bad[130 - 1] == '8' ? '9' : '8';                                /* a y that does not match x */
    CHECK(!enc_parse_pubkey(bad, &q, q33, err, sizeof err));
}

static void test_aad_and_seal(void)
{
    u8 aad[PZ_SEAL_AAD_MAX];
    int n = enc_aad(aad, sizeof aad, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 71);
    CHECK(n == 34 + 1 + 2 && aad[34] == 0 && !memcmp(aad + 35, "71", 2));
    n = enc_aad(aad, sizeof aad, "bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq", 0);
    CHECK(n == 42 + 1 + 1 && !memcmp(aad + 43, "0", 1));
    char longaddr[200];
    memset(longaddr, 'a', 199);
    longaddr[199] = 0;
    CHECK(enc_aad(aad, sizeof aad, longaddr, 0) == -1);
    CHECK(enc_aad(aad, 10, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 0) == -1);

    pz_u256 d = {{0x1122334455667788ULL, 99, 0, 0}}, r;
    pz_ge q;
    CHECK(pz_ec_mul_g(&q, &d));
    CHECK(enc_random_scalar(&r) && !pz_u256_is_zero(&r) && pz_u256_cmp(&r, &pz_secp256k1_n) < 0);
    char err[200], t1[PZ_SEAL_TEXT_BUF], t2[PZ_SEAL_TEXT_BUF];
    CHECK(enc_check(&q, err, sizeof err));
    u8 key[32], back[32], pay[PZ_SEAL_PAYLOAD_LEN];
    for (int i = 0; i < 32; i++) key[i] = (u8)(i * 9 + 1);
    CHECK(enc_seal_key(&q, key, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 71, t1, sizeof t1));
    CHECK(enc_seal_key(&q, key, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 71, t2, sizeof t2));
    CHECK(strcmp(t1, t2) != 0);                                                    /* a new random scalar each time */
    n = enc_aad(aad, sizeof aad, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 71);
    CHECK(pz_seal_decode(pay, t1) == PZ_SEAL_OK && pz_seal_open(back, pay, &d, aad, (size_t)n) == PZ_SEAL_OK && !memcmp(back, key, 32));
    CHECK(pz_seal_decode(pay, t2) == PZ_SEAL_OK && pz_seal_open(back, pay, &d, aad, (size_t)n) == PZ_SEAL_OK && !memcmp(back, key, 32));
    n = enc_aad(aad, sizeof aad, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 72);
    CHECK(pz_seal_open(back, pay, &d, aad, (size_t)n) == PZ_SEAL_E_TAG);          /* another puzzle: refused */
    CHECK(!enc_seal_key(&q, key, "x", 1, t1, 20) && t1[0] == 0);                   /* a buffer that is too small: nothing */
}

static const char *const OLD_CONF =
    "# pzsolve campaign (do not edit by hand)\nformat=1\ncreated_with=1.20.1\ncreated_at=1700000000\nname=t\npuzzle=20\nrange_start=80000\nrange_end=fffff\n"
    "address=1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH\nhash160=751e76e8199196d454941c45d1b3a323f1433bd6\npubkey=\nmode=linear\n"
    "seed=0000000000000000000000000000000000000000000000000000000000000001\nchunk_bits=20\ndp_bits=0\nbsgs_entries=0\npart=\n";

static void test_conf(void)
{
    char p[800], err[300];
    snprintf(p, sizeof p, "%s/campaign.conf", g_dir);
    write_text("campaign.conf", OLD_CONF);
    campaign_cfg c;
    CHECK(cfg_load(&c, p, err, sizeof err) == 0 && !c.has_encrypt);                 /* a file of an earlier version: no encryption */
    pz_ge q;
    u8 q33[33];
    CHECK(enc_parse_pubkey("02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5", &q, q33, err, sizeof err));
    c.encrypt_pubkey = q;
    memcpy(c.encrypt_pubkey33, q33, 33);
    c.has_encrypt = true;
    CHECK(cfg_add_encrypt_pubkey(&c, p) == 0);
    char *t = read_text("campaign.conf");
    CHECK(t && !strncmp(t, OLD_CONF, strlen(OLD_CONF)) && strstr(t, "\nencrypt_pubkey=02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5\n"));
    free(t);
    campaign_cfg d;
    CHECK(cfg_load(&d, p, err, sizeof err) == 0 && d.has_encrypt && !memcmp(d.encrypt_pubkey33, q33, 33) && pz_ge_equal(&d.encrypt_pubkey, &q));
    CHECK(!d.has_pubkey && !strcmp(d.name, "t") && d.puzzle == 20);                  /* the field is not the pubkey of the target */
    /* cfg_save writes it back, and writes nothing for a campaign without it */
    char p2[800];
    snprintf(p2, sizeof p2, "%s/second.conf", g_dir);
    CHECK(cfg_save(&d, p2) == 0);
    t = read_text("second.conf");
    CHECK(t && strstr(t, "encrypt_pubkey=02c6047f") != NULL);
    free(t);
    campaign_cfg e;
    CHECK(cfg_load(&e, p2, err, sizeof err) == 0 && e.has_encrypt);
    d.has_encrypt = false;
    CHECK(cfg_save(&d, p2) == 0);
    t = read_text("second.conf");
    CHECK(t && strstr(t, "encrypt") == NULL);
    free(t);
    /* an unusable value: the campaign is not opened (it would run without encrypting) */
    static const char *const bad[] = {"zz", "02", "0200000000000000000000000000000000000000000000000000000000000000fe"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char text[1200];
        snprintf(text, sizeof text, "%sencrypt_pubkey=%s\n", OLD_CONF, bad[i]);
        write_text("bad.conf", text);
        snprintf(p2, sizeof p2, "%s/bad.conf", g_dir);
        CHECK(cfg_load(&e, p2, err, sizeof err) != 0 && strstr(err, "encrypt_pubkey"));
    }
    write_text("empty.conf", "x");
    snprintf(p2, sizeof p2, "%s/empty.conf", g_dir);
    CHECK(cfg_add_encrypt_pubkey(&c, p2) == 0);                                      /* a file that does not end in a newline */
    t = read_text("empty.conf");
    CHECK(t && !strncmp(t, "x\nencrypt_pubkey=", 16));
    free(t);
}

/* standard input from a file */
static bool stdin_from(const char *name)
{
    char p[800];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
#ifdef _WIN32
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    return h != INVALID_HANDLE_VALUE && SetStdHandle(STD_INPUT_HANDLE, h);
#else
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    const bool ok = dup2(fileno(f), 0) == 0;
    fclose(f);
    return ok;
#endif
}

static void test_secret_from_stdin(void)
{
    write_text("in1.txt", "first secret\r\nsecond\nthird without end");
    CHECK(stdin_from("in1.txt") && !pz_stdin_is_terminal());
    char buf[32];
    CHECK(pz_read_secret("Prompt: ", buf, sizeof buf, false) == 12 && !strcmp(buf, "first secret"));    /* the CR of a Windows file is dropped */
    CHECK(pz_read_secret("Prompt: ", buf, sizeof buf, false) == 6 && !strcmp(buf, "second"));            /* the next line: nothing was read ahead */
    CHECK(pz_read_secret("Prompt: ", buf, sizeof buf, false) == 17 && !strcmp(buf, "third without end"));
    CHECK(pz_read_secret("Prompt: ", buf, sizeof buf, false) == -1 && buf[0] == 0);                       /* the end of the input */
    char big[2000];
    memset(big, 'k', 1500);
    memcpy(big + 1500, "\nnext\n", 7);
    write_text("in2.txt", big);
    CHECK(stdin_from("in2.txt"));
    memset(buf, 'z', sizeof buf);
    CHECK(pz_read_secret(NULL, buf, sizeof buf, false) == -2);                                            /* too long: refused, wiped, the line is gone */
    int allzero = 1;
    for (size_t i = 0; i < sizeof buf; i++) allzero &= buf[i] == 0;
    CHECK(allzero);
    CHECK(pz_read_secret(NULL, buf, sizeof buf, false) == 4 && !strcmp(buf, "next"));
    write_text("in3.txt", "\n");
    CHECK(stdin_from("in3.txt") && pz_read_secret(NULL, buf, sizeof buf, false) == 0 && buf[0] == 0);    /* an empty line is a line */
    /* the length of a password in characters is counted by the callers; here only that bytes pass unchanged */
    write_text("in4.txt", "p\xc3\xa7\xc3\xa3o\n");
    CHECK(stdin_from("in4.txt") && pz_read_secret(NULL, buf, sizeof buf, false) == 6 && !memcmp(buf, "p\xc3\xa7\xc3\xa3o", 6));
}

static void test_run_shell(void)
{
    char out[800], cmd[1000], why[200];
    int code = -1;
    snprintf(out, sizeof out, "%s/hook.out", g_dir);
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "echo %%PZ_A%%-%%PZ_B%%>\"%s\"", out);
#else
    snprintf(cmd, sizeof cmd, "printf '%%s-%%s' \"$PZ_A\" \"$PZ_B\" > '%s'", out);
#endif
    const char *const env[] = {"PZ_A=alpha", "PZ_B=beta gamma", NULL};
    CHECK(pz_run_shell(cmd, env, 20.0, &code, why, sizeof why) == 0 && code == 0);
    char *t = pz_read_file(out, NULL);
    CHECK(t && !strncmp(t, "alpha-beta gamma", 16));
    free(t);
    CHECK(pz_run_shell("exit 7", NULL, 20.0, &code, why, sizeof why) == 0 && code == 7);
#ifndef _WIN32
    CHECK(pz_run_shell("exit 0", NULL, 20.0, &code, why, sizeof why) == 0 && code == 0);
    const double t0 = pz_mono();
    CHECK(pz_run_shell("sleep 30", NULL, 0.5, &code, why, sizeof why) == 1);       /* the time is up: stopped */
    CHECK(pz_mono() - t0 < 10);
    /* a variable of the environment is replaced, not repeated */
    setenv("PZ_A", "old", 1);
    snprintf(cmd, sizeof cmd, "printf '%%s' \"$PZ_A\" > '%s'", out);
    CHECK(pz_run_shell(cmd, env, 20.0, &code, why, sizeof why) == 0);
    t = pz_read_file(out, NULL);
    CHECK(t && !strcmp(t, "alpha"));
    free(t);
#else
    const double t0 = pz_mono();
    CHECK(pz_run_shell("ping -n 30 127.0.0.1 >nul", NULL, 0.5, &code, why, sizeof why) == 1);
    CHECK(pz_mono() - t0 < 20);
#endif
}

static void test_onfound(void)
{
    char out[800], cmd[1100];
    snprintf(out, sizeof out, "%s/onfound.out", g_dir);
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "echo %%PZ_FOUND_SEALED%%;%%PZ_FOUND_ADDRESS%%;%%PZ_FOUND_PUZZLE%%;%%PZ_FOUND_CAMPAIGN%%;%%PZ_FOUND_FILE%%>>\"%s\"", out);
#else
    snprintf(cmd, sizeof cmd, "printf '%%s;%%s;%%s;%%s;%%s\\n' \"$PZ_FOUND_SEALED\" \"$PZ_FOUND_ADDRESS\" \"$PZ_FOUND_PUZZLE\" \"$PZ_FOUND_CAMPAIGN\" \"$PZ_FOUND_FILE\" >> '%s'", out);
#endif
    onfound_wait();                                            /* nothing started: nothing to wait for */
    onfound_start(cmd, "PZENC1:abc", "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 71, "p71", "/x/FOUND.txt");
    onfound_start(cmd, "PZENC1:other", "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", 71, "p71", "/x/FOUND.txt");      /* once only */
    onfound_wait();
    char *t = pz_read_file(out, NULL);
    const char *want = "PZENC1:abc;1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH;71;p71;/x/FOUND.txt";
    CHECK(t && !strncmp(t, want, strlen(want)));
    int lines = 0;
    for (const char *q = t ? t : ""; *q; q++) lines += *q == '\n';
    CHECK(lines == 1);
    free(t);
}

static void test_harden(void)
{
    char why[200];
    CHECK(pz_harden_process(why, sizeof why) == 
#ifdef _WIN32
          false
#else
          true
#endif
    );
#ifdef _WIN32
    CHECK(why[0]);                                              /* it says what it cannot do */
#endif
#if !defined(_WIN32)
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 0 && rl.rlim_max == 0);
#endif
#ifdef __linux__
    CHECK(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0);
#endif
    char buf[100];
    CHECK(pz_mem_lock(buf, 0));                                  /* nothing to lock is not a failure */
    pz_mem_lock(buf, sizeof buf);                                /* may be refused: no check */
    pz_mem_unlock(buf, sizeof buf);
}

int main(void)
{
    pz_env_init();
    pz_log_set_quiet(1);
    if (!pz_make_temp_dir(g_dir, sizeof g_dir, "pzenct")) { puts("t_encrypt: no temporary folder"); return 2; }
    test_pubkey();
    test_aad_and_seal();
    test_conf();
    test_secret_from_stdin();
    test_run_shell();
    test_onfound();
    test_harden();
    pz_remove_tree(g_dir);
    puts(fails ? "t_encrypt: FAILED" : "t_encrypt: OK");
    return fails != 0;
}
