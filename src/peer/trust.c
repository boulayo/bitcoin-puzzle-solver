/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* trust.c - the signatures of the maintainers (see trust.h). */
#include "trust.h"
#include <pzcore/seal.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The maintainers of the public networks and how many of them must sign. None yet: until there are, only test networks run (PZNET_TRUST). */
static const char *const MAINTAINERS[] = {NULL};
#define MAINTAINERS_THRESHOLD 2

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool unhex32(const char *s, size_t len, uint8_t out[32])
{
    if (len != 64) return false;
    for (int i = 0; i < 32; i++) {
        const int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static bool add_key(trust_set *t, const char *s, size_t len)
{
    if (t->n == TRUST_MAX || !unhex32(s, len, t->key[t->n])) return false;
    for (int i = 0; i < t->n; i++)
        if (!memcmp(t->key[i], t->key[t->n], 32)) return true;   /* the same key twice counts once */
    t->n++;
    return true;
}

bool trust_load(trust_set *t, char *err, size_t errlen)
{
    memset(t, 0, sizeof *t);
    const char *env = getenv("PZNET_TRUST");
    if (env && *env) {
        for (const char *p = env; *p;) {
            const size_t len = strcspn(p, ",");
            if (!add_key(t, p, len)) return snprintf(err, errlen, "PZNET_TRUST: expected x-only keys of 64 hex digits separated by commas"), false;
            p += len + (p[len] == ',');
        }
        const char *th = getenv("PZNET_THRESHOLD");
        t->threshold = th && *th ? atoi(th) : 1;
    } else {
        for (int i = 0; MAINTAINERS[i]; i++) (void)add_key(t, MAINTAINERS[i], strlen(MAINTAINERS[i]));
        t->threshold = MAINTAINERS_THRESHOLD;
    }
    if (!t->n) return snprintf(err, errlen, "no maintainer keys in this version: a test network needs PZNET_TRUST"), false;
    if (t->threshold < 1 || t->threshold > t->n) return snprintf(err, errlen, "PZNET_THRESHOLD must be 1 to %d", t->n), false;
    return true;
}

bool trust_check(const secp256k1_context *ctx, const trust_set *t, const netfile *nf, int *valid)
{
    uint8_t msg[32];
    bool used[TRUST_MAX] = {false};
    netfile_sig_msg(nf, msg);
    *valid = 0;
    for (int s = 0; s < nf->nsig; s++)
        for (int k = 0; k < t->n; k++) {
            secp256k1_xonly_pubkey xo;
            if (used[k] || memcmp(t->key[k], nf->sig_key[s], 32) || !secp256k1_xonly_pubkey_parse(ctx, &xo, t->key[k])) continue;
            if (secp256k1_schnorrsig_verify(ctx, nf->sig[s], msg, 32, &xo)) used[k] = true, (*valid)++;
        }
    return *valid >= t->threshold;
}

bool trust_sign(const secp256k1_context *ctx, const uint8_t sec[32], const netfile *nf, char line[200])
{
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xo;
    uint8_t msg[32], x[32], sig[64];
    netfile_sig_msg(nf, msg);
    const bool ok = secp256k1_keypair_create(ctx, &kp, sec) && secp256k1_keypair_xonly_pub(ctx, &xo, NULL, &kp) &&
                    secp256k1_xonly_pubkey_serialize(ctx, x, &xo) && secp256k1_schnorrsig_sign32(ctx, sig, msg, &kp, NULL);
    pz_secure_zero(&kp, sizeof kp);
    if (!ok) return false;
    int o = snprintf(line, 200, "sig=");
    for (int i = 0; i < 32; i++) o += snprintf(line + o, 200 - (size_t)o, "%02x", x[i]);
    o += snprintf(line + o, 200 - (size_t)o, ":");
    for (int i = 0; i < 64; i++) o += snprintf(line + o, 200 - (size_t)o, "%02x", sig[i]);
    return true;
}
