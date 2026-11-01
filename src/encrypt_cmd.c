/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* encrypt_cmd.c - `encrypt-keygen` (the public key to encrypt to) and `decrypt-key` (the key of a campaign that encrypted it). See encrypt.h. */
#include "encrypt.h"
#include "encrypt_cmd.h"
#include "platform.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>

enum { EC_EXIT_NOT_OPENED = 1, EC_EXIT_ERROR = 2 };      /* 0: the key was recovered and checked; 1: the secret does not open it; 2: the input or the arguments */

int encrypt_keygen_main(int argc, char **argv)
{
    static const struct option lo[] = {{"random", 0, 0, 'R'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    bool random = false;
    optind = 1;
    opterr = 0;
    int c;
    while ((c = getopt_long(argc, argv, ":h", lo, NULL)) != -1) {
        if (c == 'R') random = true;
        else if (c == 'h') { pz_help("encrypt-keygen", stdout); return 0; }
        else pz_usage_error("encrypt-keygen", "unrecognized option '%s'", optind > 0 && optind <= argc ? argv[optind - 1] : "?");
    }
    if (optind < argc) pz_usage_error("encrypt-keygen", "unexpected argument '%s'", argv[optind]);
    pz_ge q;
    u8 q33[33];
    char hx[67], id[9];
    if (random) {
        u8 b[32];
        pz_u256 d;
        if (!pz_os_random(b, sizeof b)) pz_die("the system gives no random numbers");
        pz_seal_scalar_from_bytes(&d, b);
        pz_secure_zero(b, sizeof b);
        if (!pz_ec_mul_g(&q, &d)) pz_die("internal error: the random key is not usable");
        pz_ge_to_pub33(q33, &q);
        pz_hex_encode(q33, 33, hx);
        enc_key_id(&q, id);
        u8 kb[32];
        char sk[65];
        pz_u256_to_be(&d, kb);
        pz_hex_encode(kb, 32, sk);                                                         /* always 64 digits */
        printf("private key:  %s\npublic key:   %s\nkey id:       %s\n\n", sk, hx, id);
        printf("Store the private key like a secret, in a place other than this screen and the rented computer: whoever has it can open every key that\n"
               "was encrypted to this public key, and if it is lost those keys are lost. 'pzsolve decrypt-key --raw-key' reads it.\n"
               "Give the public key to 'pzsolve hunt --encrypt-pubkey'.\n");
        pz_secure_zero(sk, sizeof sk);
        pz_secure_zero(kb, sizeof kb);
        pz_secure_zero(&d, sizeof d);
        return 0;
    }
    enc_password_to_pubkey(false, &q, q33);
    pz_hex_encode(q33, 33, hx);
    enc_key_id(&q, id);
    printf("public key:   %s\nkey id:       %s\n\n", hx, id);
    printf("Give the public key to 'pzsolve hunt --encrypt-pubkey' (or type the same password for 'hunt --encrypt-password' on a computer you trust).\n"
           "At home 'pzsolve decrypt-key' asks for the same password. Nothing was written to disk.\n\n");
    enc_print_password_advice(stdout);
    return 0;
}

/* ---- decrypt-key ---- */

#define IN_MAX (1 << 20)

static char *read_all_stdin(void)
{
    size_t cap = 4096, n = 0;
    char *b = pz_xmalloc(cap);
    for (;;) {
        const ssize_t k = read(0, b + n, cap - n - 1);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        n += (size_t)k;
        if (n + 1 >= cap) {
            if (cap >= IN_MAX) break;
            cap *= 2;
            b = pz_xrealloc(b, cap);
        }
    }
    b[n] = 0;
    return b;
}

static bool is_b58(char c) { return c && strchr("123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz", c) != NULL; }

/* The value of the first line "name: value" of the text (name at the start of the line, blanks allowed before it), up to a blank; false if there is none. */
static bool line_value(const char *text, const char *name, char *out, size_t cap)
{
    const size_t nl = strlen(name);
    for (const char *p = text; *p;) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, name, nl) && q[nl] == ':') {
            q += nl + 1;
            while (*q == ' ' || *q == '\t') q++;
            size_t n = 0;
            while (q[n] && !isspace((unsigned char)q[n])) n++;
            if (n && n < cap) { memcpy(out, q, n); out[n] = 0; return true; }
        }
        const char *e = strchr(p, '\n');
        if (!e) break;
        p = e + 1;
    }
    return false;
}

typedef struct { char text[PZ_SEAL_TEXT_BUF + 8]; int line; u8 payload[PZ_SEAL_PAYLOAD_LEN]; bool ok; } token_t;

int decrypt_key_main(int argc, char **argv)
{
    static const struct option lo[] = {{"raw-key", 0, 0, 'r'}, {"address", 1, 0, 'a'}, {"puzzle", 1, 0, 'p'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    bool raw = false;
    char opt_addr[80] = "";
    int opt_puzzle = -1;
    optind = 1;
    opterr = 0;
    int c;
    while ((c = getopt_long(argc, argv, ":h", lo, NULL)) != -1) {
        switch (c) {
        case 'r': raw = true; break;
        case 'a':
            if (strlen(optarg) >= sizeof opt_addr) pz_die("--address: the value is too long");
            memcpy(opt_addr, optarg, strlen(optarg) + 1);
            break;
        case 'p': opt_puzzle = (int)pz_arg_int("--puzzle", optarg, 0, 256); break;
        case 'h': pz_help("decrypt-key", stdout); return 0;
        case ':': pz_usage_error("decrypt-key", "the option '%s' needs a value", argv[optind - 1]);
        default: pz_usage_error("decrypt-key", "unrecognized option '%s'", optind > 0 && optind <= argc ? argv[optind - 1] : "?");
        }
    }
    if (argc - optind > 1) pz_usage_error("decrypt-key", "at most one file (or - for standard input)");
    const char *file = optind < argc ? argv[optind] : NULL;
    if (!file && access("FOUND.txt", R_OK) == 0) file = "FOUND.txt";
    char *text;
    const bool from_stdin = !file || !strcmp(file, "-");
    if (from_stdin) {
        if (pz_stdin_is_terminal())
            fputs("Paste the line 'PZENC1:...' (and the lines 'address:' and 'puzzle:' of FOUND.txt if you have them), then end the input with Ctrl-D\n"
                  "(Ctrl-Z and Enter on Windows):\n", stderr);
        text = read_all_stdin();
    } else {
        size_t len = 0;
        text = pz_read_file(file, &len);
        if (!text) pz_die("cannot read %s: %s", file, strerror(errno));
        if (len > IN_MAX) pz_die("%s is too large for a file of encrypted keys", file);
    }
    /* the tokens */
    token_t *tok = pz_xcalloc(64, sizeof *tok);
    int nt = 0, bad = 0, line = 1;
    for (const char *p = text; *p;) {
        if (*p == '\n') { line++; p++; continue; }
        if (strncmp(p, PZ_SEAL_TEXT_PREFIX, PZ_SEAL_TEXT_PREFIX_LEN) != 0) { p++; continue; }
        size_t n = PZ_SEAL_TEXT_PREFIX_LEN;
        while (is_b58(p[n])) n++;
        if (nt < 64) {
            token_t *t = &tok[nt++];
            t->line = line;
            snprintf(t->text, sizeof t->text, "%.*s", (int)(n < sizeof t->text - 1 ? n : sizeof t->text - 1), p);
            const pz_seal_status st = n < sizeof t->text - 1 ? pz_seal_decode(t->payload, t->text) : PZ_SEAL_E_FORMAT;
            if (st == PZ_SEAL_OK) t->ok = true;
            else {
                bad++;
                fprintf(stderr, "line %d: the encrypted key is damaged or incomplete: the check characters at its end do not match (a cut or mistyped copy; copy it again)\n", line);
            }
        }
        p += n;
    }
    if (nt == 0) {
        free(text);
        free(tok);
        fprintf(stderr, "no encrypted key (a token that starts with PZENC1:) was found in %s\n", from_stdin ? "the input" : file);
        return EC_EXIT_ERROR;
    }
    int usable = 0;
    for (int i = 0; i < nt; i++) {
        if (!tok[i].ok) continue;
        u8 kid[4];
        char idh[9];
        const pz_seal_status st = pz_seal_peek(tok[i].payload, kid);
        if (st == PZ_SEAL_E_VERSION) {
            fprintf(stderr, "line %d: version %d of the format, which this program does not know (it reads version %d): use a newer pzsolve\n", tok[i].line, tok[i].payload[0], PZ_SEAL_VERSION);
            tok[i].ok = false;
            bad++;
        } else if (st != PZ_SEAL_OK) {
            fprintf(stderr, "line %d: the encrypted key is damaged (the point inside it is not valid)\n", tok[i].line);
            tok[i].ok = false;
            bad++;
        } else {
            pz_hex_encode(kid, 4, idh);
            printf("line %d: encrypted to the public key with id %s\n", tok[i].line, idh);
            usable++;
        }
    }
    /* the address and the puzzle: part of what was encrypted */
    char address[96] = "", pzs[16] = "";
    if (opt_addr[0]) memcpy(address, opt_addr, strlen(opt_addr) + 1);
    else line_value(text, "address", address, sizeof address);
    int puzzle = 0;
    if (opt_puzzle >= 0) puzzle = opt_puzzle;
    else if (line_value(text, "puzzle", pzs, sizeof pzs)) puzzle = pz_atoi(pzs);
    free(text);
    if (!usable) {
        free(tok);
        return EC_EXIT_ERROR;
    }
    u8 h160[20];
    if (!address[0]) {
        free(tok);
        fprintf(stderr, "the address (and the puzzle) the key was found for are needed: they were encrypted with the key. They are in FOUND.txt on the lines 'address:' and 'puzzle:'; "
                        "give the whole file, or give them with --address and --puzzle\n");
        return EC_EXIT_ERROR;
    }
    if (!pz_address_to_hash160(address, h160)) {
        free(tok);
        fprintf(stderr, "'%s' is not a main-network address (a \"1...\" or \"bc1q...\" address)\n", address);
        return EC_EXIT_ERROR;
    }
    u8 aad[PZ_SEAL_AAD_MAX];
    const int al = enc_aad(aad, sizeof aad, address, puzzle);
    if (al < 0) { free(tok); fprintf(stderr, "the address is too long\n"); return EC_EXIT_ERROR; }

    /* the secret */
    char sec[ENC_PASSWORD_MAX + 1];
    memset(sec, 0, sizeof sec);
    pz_mem_lock(sec, sizeof sec);
    pz_u256 d;
    memset(&d, 0, sizeof d);
    const int n = pz_read_secret(raw ? "Private key (64 hexadecimal digits): " : "Password: ", sec, sizeof sec, from_stdin);
    const char *fail = NULL;
    if (n == -2) fail = "the line is too long";
    else if (n < 0) fail = from_stdin && !pz_stdin_is_terminal() ? "the secret cannot be read: the input was used for the text and there is no terminal; give the file as an argument and the secret as the first line of standard input"
                                                                : "no secret was read";
    else if (raw) {
        if (!pz_u256_from_hex(&d, sec) || pz_u256_is_zero(&d) || pz_u256_cmp(&d, &pz_secp256k1_n) >= 0) fail = "that is not a private key (1 to 64 hexadecimal digits, from 1 to the order of the curve minus 1)";
    } else if (n == 0) fail = "an empty password cannot be right";
    else {
        fputs("Deriving the key from the password (about a second)...\n", stderr);
        if (!enc_key_from_password(&d, sec, (size_t)n)) fail = "not enough memory for the key derivation (256 MiB)";
    }
    pz_secure_zero(sec, sizeof sec);
    pz_mem_unlock(sec, sizeof sec);
    if (fail) {
        fprintf(stderr, "%s\n", fail);
        pz_secure_zero(&d, sizeof d);
        free(tok);
        return EC_EXIT_ERROR;
    }
    pz_ge q;
    char mine[9];
    if (!pz_ec_mul_g(&q, &d)) { pz_secure_zero(&d, sizeof d); free(tok); fprintf(stderr, "that is not a usable key\n"); return EC_EXIT_ERROR; }
    enc_key_id(&q, mine);
    int recovered = 0, failed = 0;
    for (int i = 0; i < nt; i++) {
        if (!tok[i].ok) continue;
        char theirs[9];
        pz_hex_encode(tok[i].payload + 1, 4, theirs);
        if (strcmp(mine, theirs) != 0) {            /* said before anything is tried: a wrong password is the usual mistake */
            fprintf(stderr, "line %d: encrypted to the public key with id %s, but this %s gives id %s: wrong %s\n", tok[i].line, theirs,
                    raw ? "private key" : "password", mine, raw ? "key" : "password");
            failed++;
            continue;
        }
        u8 key[32];
        const pz_seal_status st = pz_seal_open(key, tok[i].payload, &d, aad, (size_t)al);
        if (st == PZ_SEAL_E_TAG) {
            fprintf(stderr, "line %d: the key id is right but the authentication failed: the encrypted key was altered, or it was found for another address or puzzle than %s / %d\n",
                    tok[i].line, address, puzzle);
            failed++;
            continue;
        }
        if (st != PZ_SEAL_OK) {
            fprintf(stderr, "line %d: the encrypted key cannot be opened (status %d)\n", tok[i].line, (int)st);
            failed++;
            continue;
        }
        pz_u256 k;
        pz_ge kp;
        u8 pub[33], hk[20];
        pz_u256_from_be(&k, key);
        const bool good = pz_ec_mul_g(&kp, &k) && (pz_ge_to_pub33(pub, &kp), pz_hash160(pub, 33, hk), memcmp(hk, h160, 20) == 0);
        if (!good) {
            fprintf(stderr, "line %d: CHECK FAILED: the key that came out does not belong to the address %s (nothing is shown)\n", tok[i].line, address);
            failed++;
        } else {
            char hx[PZ_U256_HEX_BUF], wif[PZ_WIF_BUF];
            pz_u256_to_hex(&k, hx);
            pz_privkey_to_wif(key, true, wif, sizeof wif);
            printf("\nKey recovered and checked (line %d)\n  key (hex):      0x%s\n  WIF compressed: %s\n  address:        %s\n", tok[i].line, hx, wif, address);
            if (puzzle) printf("  puzzle:         #%d\n", puzzle);
            printf("  check:          OK, the key belongs to the address\n");
            pz_secure_zero(hx, sizeof hx);
            pz_secure_zero(wif, sizeof wif);
            recovered++;
        }
        pz_secure_zero(key, sizeof key);
        pz_secure_zero(&k, sizeof k);
    }
    pz_secure_zero(&d, sizeof d);
    free(tok);
    if (recovered == 0) return failed ? EC_EXIT_NOT_OPENED : EC_EXIT_ERROR;
    return failed || bad ? EC_EXIT_NOT_OPENED : 0;
}
