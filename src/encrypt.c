/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* encrypt.c - see encrypt.h. */
#include "encrypt.h"
#include "platform.h"
#include "envopts.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>

bool enc_parse_pubkey(const char *hex, pz_ge *q, u8 q33[33], char *err, size_t errn)
{
    u8 b[65];
    const size_t l = strlen(hex);
    if (l != 66 && l != 130) {
        snprintf(err, errn, "expected 66 hexadecimal digits (a compressed public key) or 130 (uncompressed), got %zu characters", l);
        return false;
    }
    if (pz_hex_decode(hex, b, sizeof b) != (int)(l / 2)) {
        snprintf(err, errn, "not a hexadecimal number");
        return false;
    }
    pz_ge p;
    if (!pz_ge_from_pubkey(&p, b, l / 2)) {
        snprintf(err, errn, "not a point of the secp256k1 curve (a public key starts with 02 or 03, or 04 when uncompressed)");
        return false;
    }
    *q = p;
    pz_ge_to_pub33(q33, &p);
    return true;
}

void enc_key_id(const pz_ge *q, char id[9])
{
    u8 k[4];
    pz_seal_key_id(q, k);
    pz_hex_encode(k, 4, id);
}

unsigned enc_log2n(void)
{
    static bool warned;
    if (!g_env.test_seal_log2n) return PZ_SEAL_SCRYPT_LOG2N;
    if (!warned) {
        warned = true;
        fprintf(stderr, "WARNING: PZ_TEST_SEAL_LOG2N=%d: the key of the password is derived at a test cost (2^%d instead of 2^%d): a password protected this way is weak.\n"
                        "         This variable is for the tests of the program only.\n", g_env.test_seal_log2n, g_env.test_seal_log2n, PZ_SEAL_SCRYPT_LOG2N);
    }
    return (unsigned)g_env.test_seal_log2n;
}

bool enc_random_scalar(pz_u256 *r)
{
    for (int i = 0; i < 64; i++) {
        u8 b[32];
        if (!pz_os_random(b, sizeof b)) return false;
        pz_u256_from_be(r, b);
        pz_secure_zero(b, sizeof b);
        if (!pz_u256_is_zero(r) && pz_u256_cmp(r, &pz_secp256k1_n) < 0) return true;   /* otherwise draw again, as the format asks */
    }
    return false;
}

bool enc_seal_key(const pz_ge *q, const u8 key[32], char *text, size_t cap)
{
    u8 pay[PZ_SEAL_PAYLOAD_LEN];
    if (cap) text[0] = 0;
    pz_u256 r;
    if (!enc_random_scalar(&r)) return false;
    const pz_seal_status st = pz_seal(pay, q, key, &r);
    pz_secure_zero(&r, sizeof r);
    if (st != PZ_SEAL_OK) return false;
    return pz_seal_encode(pay, text, cap) > 0;
}

bool enc_check(const pz_ge *q, char *err, size_t errn)
{
    u8 dummy[32], pay[PZ_SEAL_PAYLOAD_LEN];
    pz_u256 r;
    if (!enc_random_scalar(&r)) {
        snprintf(err, errn, "the system gives no random numbers: the key could not be encrypted when it is found");
        return false;
    }
    memset(dummy, 0x5a, sizeof dummy);
    if (pz_seal(pay, q, dummy, &r) != PZ_SEAL_OK) {
        snprintf(err, errn, "the public key cannot be used to encrypt");
        return false;
    }
    pz_secure_zero(&r, sizeof r);
    char text[PZ_SEAL_TEXT_BUF];
    u8 dec[PZ_SEAL_PAYLOAD_LEN];
    if (pz_seal_encode(pay, text, sizeof text) < 0 || pz_seal_decode(dec, text) != PZ_SEAL_OK || memcmp(dec, pay, sizeof dec) != 0) {
        snprintf(err, errn, "internal error: the text form of an encrypted key does not read back");
        return false;
    }
    return true;
}

bool enc_key_from_password(pz_u256 *d, const char *pw, size_t pwlen)
{
    const unsigned lg = enc_log2n();
    const size_t ws = pz_scrypt_work_size(1ULL << lg, PZ_SEAL_SCRYPT_R, PZ_SEAL_SCRYPT_P);
    void *work = malloc(ws);
    if (!work) return false;
    if (!pz_mem_lock(work, ws)) pz_log(PZ_LOG_DEBUG, "the memory of the key derivation could not be locked in RAM (limit on locked memory): it may be swapped out");
    const bool ok = pz_seal_key_from_password(d, (const u8 *)pw, pwlen, lg, work, ws);
    pz_secure_zero(work, ws);
    pz_mem_unlock(work, ws);
    free(work);
    return ok;
}

void enc_print_password_advice(FILE *f)
{
    fputs("The password is all that protects the encrypted key: anyone who knows the public key can try passwords against it offline, as fast as\n"
          "their computers allow (the derivation costs about a second and 256 MiB per guess, which slows a guess down but does not stop one).\n"
          "Use 7 or more random words (diceware: about 90 bits) or 20 or more random characters. If you lose the password, the key is lost.\n", f);
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

void enc_password_to_pubkey(bool hunt, pz_ge *q, u8 q33[33])
{
    char pw[ENC_PASSWORD_MAX + 1], pw2[ENC_PASSWORD_MAX + 1];
    pz_u256 d;
    memset(pw, 0, sizeof pw);
    memset(pw2, 0, sizeof pw2);
    memset(&d, 0, sizeof d);
    if (!pz_mem_lock(pw, sizeof pw) || !pz_mem_lock(pw2, sizeof pw2)) pz_log(PZ_LOG_DEBUG, "the password buffers could not be locked in RAM (limit on locked memory)");
    const bool tty = pz_stdin_is_terminal();
    if (hunt) {
        fputs("The key that this campaign finds will be encrypted to a key made from a password that you type now.\n"
              "Use this only on a computer you trust while you type: whatever sees the keyboard, the terminal or the memory of this program at this\n"
              "moment can capture the password and then open the encrypted key. It protects what happens AFTER this first step (an unattended machine,\n"
              "files, backups, disks returned after the rental), not a hostile machine from the start. The safer way is 'pzsolve encrypt-keygen' on a\n"
              "computer you trust and 'hunt --encrypt-pubkey'.\n", stderr);
    }
    const int n = pz_read_secret("Password: ", pw, sizeof pw, false);
    const char *fail = NULL;
    if (n == -2) fail = "the password is too long (at most 1024 bytes)";
    else if (n < 0) fail = tty ? "no password was read" : "no password: standard input has no line (give the password as the first line)";
    else if (n == 0) fail = "an empty password is refused";
    if (!fail && tty) {
        const int m = pz_read_secret("Repeat the password: ", pw2, sizeof pw2, false);
        if (m < 0 || (size_t)m != (size_t)n || !pz_ct_equal(pw, pw2, (size_t)n)) fail = "the two passwords differ: nothing was done";
    }
    if (!fail) {
        const size_t chars = utf8_chars(pw);
        if (chars < ENC_PASSWORD_ADVISED)
            fprintf(stderr, "WARNING: the password has only %zu character%s (at least %d are advised, and 7 random words or 20 random characters are better):\n"
                            "         anyone who sees the public key can guess a short password offline.\n", chars, chars == 1 ? "" : "s", ENC_PASSWORD_ADVISED);
        fputs("Deriving the key from the password (about a second)...\n", stderr);
        if (!enc_key_from_password(&d, pw, (size_t)n)) fail = "not enough memory for the key derivation (256 MiB)";
    }
    pz_secure_zero(pw, sizeof pw);
    pz_secure_zero(pw2, sizeof pw2);
    pz_mem_unlock(pw, sizeof pw);
    pz_mem_unlock(pw2, sizeof pw2);
    if (fail) {
        pz_secure_zero(&d, sizeof d);
        pz_die("%s", fail);
    }
    if (!pz_ec_mul_g(q, &d)) pz_die("internal error: the key made from the password is not usable");
    pz_secure_zero(&d, sizeof d);
    pz_ge_to_pub33(q33, q);
}
