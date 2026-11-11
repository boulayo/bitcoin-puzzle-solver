/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* btc.c - Bitcoin transactions (btc.h). */
#include "btc.h"
#include "sys.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <secp256k1.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---- libsecp256k1 ---- */

/* One context for the whole process, made on first use; after that it is only read, which libsecp256k1 allows from any number of
 * threads. Randomizing it blinds the signing against side channels; without the entropy it still signs correctly, so a failure
 * there is ignored (and randomizing a context made here cannot fail). */
static secp256k1_context *g_ctx;
static pthread_once_t g_ctx_once = PTHREAD_ONCE_INIT;

static void ctx_make(void)
{
    uint8_t seed[32];
    g_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (sys_random(seed, sizeof seed)) (void)!secp256k1_context_randomize(g_ctx, seed);
    pz_secure_zero(seed, sizeof seed);
}

static const secp256k1_context *ctx(void)
{
    pthread_once(&g_ctx_once, ctx_make);
    return g_ctx;
}

/* ---- bytes ---- */

static uint8_t *put_le32(uint8_t *o, uint32_t v)
{
    for (int i = 0; i < 4; i++) o[i] = (uint8_t)(v >> 8 * i);
    return o + 4;
}

static uint8_t *put_le64(uint8_t *o, uint64_t v)
{
    for (int i = 0; i < 8; i++) o[i] = (uint8_t)(v >> 8 * i);
    return o + 8;
}

static uint64_t get_le(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    while (n--) v = v << 8 | p[n];
    return v;
}

/* CompactSize, the length prefix of the serialization */
static size_t size_len(uint64_t v) { return v < 0xfd ? 1 : v <= 0xffff ? 3 : v <= 0xffffffff ? 5 : 9; }

static uint8_t *put_size(uint8_t *o, uint64_t v)
{
    if (v < 0xfd) { *o = (uint8_t)v; return o + 1; }
    const size_t n = size_len(v) - 1;
    *o++ = n == 2 ? 0xfd : n == 4 ? 0xfe : 0xff;
    for (size_t i = 0; i < n; i++) o[i] = (uint8_t)(v >> 8 * i);
    return o + n;
}

/* ---- hex ---- */

void btc_hex(const uint8_t *p, size_t n, char *out)
{
    static const char X[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = X[p[i] >> 4];
        out[2 * i + 1] = X[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int hex_digit(unsigned c)
{
    if (c - '0' < 10) return (int)(c - '0');
    c |= 0x20;
    if (c - 'a' < 6) return (int)(c - 'a' + 10);
    return -1;
}

bool btc_unhex(const char *s, size_t len, uint8_t *out)
{
    if (len % 2 || (len && !s)) return false;
    for (size_t i = 0; i < len / 2; i++) {
        const int hi = hex_digit((unsigned char)s[2 * i]), lo = hex_digit((unsigned char)s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

void btc_hash_to_hex(const uint8_t h[32], char out[65])
{
    uint8_t r[32];
    for (int i = 0; i < 32; i++) r[i] = h[31 - i];
    btc_hex(r, 32, out);
}

bool btc_hash_from_hex(const char *s, uint8_t h[32])
{
    uint8_t r[32];
    if (!s || strnlen(s, 65) != 64 || !btc_unhex(s, 64, r)) return false;
    for (int i = 0; i < 32; i++) h[i] = r[31 - i];
    return true;
}

/* ---- addresses and scripts ---- */

static const struct {
    const char *name, *hrp;
    uint8_t p2pkh, p2sh;   /* the version bytes of the Base58Check forms */
} CHAINS[] = {
    [BTC_MAIN] = {"main", "bc", 0x00, 0x05},
    [BTC_TEST] = {"test", "tb", 0x6f, 0xc4},
    [BTC_SIGNET] = {"signet", "tb", 0x6f, 0xc4},
    [BTC_REGTEST] = {"regtest", "bcrt", 0x6f, 0xc4},
};

bool btc_chain_parse(const char *name, btc_chain *chain)
{
    for (size_t c = 0; name && c < sizeof CHAINS / sizeof CHAINS[0]; c++)
        if (!strcmp(name, CHAINS[c].name)) {
            *chain = (btc_chain)c;
            return true;
        }
    return false;
}

void btc_script_p2pkh(const uint8_t h160[20], uint8_t spk[BTC_P2PKH_LEN])
{
    spk[0] = 0x76;   /* OP_DUP */
    spk[1] = 0xa9;   /* OP_HASH160 */
    spk[2] = 20;
    memcpy(spk + 3, h160, 20);
    spk[23] = 0x88;  /* OP_EQUALVERIFY */
    spk[24] = 0xac;  /* OP_CHECKSIG */
}

bool btc_script_is_p2pkh(const uint8_t *spk, size_t len, uint8_t h160[20])
{
    if (!spk || len != BTC_P2PKH_LEN || spk[0] != 0x76 || spk[1] != 0xa9 || spk[2] != 20 || spk[23] != 0x88 || spk[24] != 0xac)
        return false;
    if (h160) memcpy(h160, spk + 3, 20);
    return true;
}

size_t btc_address_script(btc_chain chain, const char *addr, uint8_t spk[BTC_SPK_MAX])
{
    if ((unsigned)chain >= sizeof CHAINS / sizeof CHAINS[0] || !addr) return 0;
    uint8_t pl[21];
    if (pz_base58check_decode(addr, pl, sizeof pl) == 21) {
        if (pl[0] == CHAINS[chain].p2pkh) {
            btc_script_p2pkh(pl + 1, spk);
            return BTC_P2PKH_LEN;
        }
        if (pl[0] == CHAINS[chain].p2sh) {
            spk[0] = 0xa9;   /* OP_HASH160 <h160> OP_EQUAL */
            spk[1] = 20;
            memcpy(spk + 2, pl + 1, 20);
            spk[22] = 0x87;
            return 23;
        }
        return 0;
    }
    unsigned ver;
    uint8_t prog[40];
    size_t plen = sizeof prog;
    if (!pz_segwit_address_decode(CHAINS[chain].hrp, addr, &ver, prog, &plen)) return 0;
    /* version 0 with 20 bytes (P2WPKH) or 32 (P2WSH), the decoder allows no other; version 1 with 32 bytes, taproot. The later
     * versions are not spendable today by anyone's rules: coins sent there could be taken by anyone, so they are refused. */
    if (ver > 1 || (ver == 1 && plen != 32)) return 0;
    spk[0] = ver ? (uint8_t)(0x50 + ver) : 0;   /* OP_0 or OP_1 */
    spk[1] = (uint8_t)plen;
    memcpy(spk + 2, prog, plen);
    return 2 + plen;
}

/* ---- reading ---- */

typedef struct {
    const uint8_t *p, *end;
    bool bad;   /* a read went past the end, or a length was malformed: every later read fails too */
} rd;

static const uint8_t *rd_take(rd *r, uint64_t n)
{
    if (r->bad || (uint64_t)(r->end - r->p) < n) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}

static uint64_t rd_le(rd *r, size_t n)
{
    const uint8_t *p = rd_take(r, n);
    return p ? get_le(p, n) : 0;
}

/* a CompactSize as the node reads it: in its shortest form and at most 0x02000000 (MAX_SIZE of Bitcoin Core) */
static uint64_t rd_size(rd *r)
{
    const uint64_t b = rd_le(r, 1);
    if (b < 0xfd) return b;
    const size_t n = b == 0xfd ? 2 : b == 0xfe ? 4 : 8;
    const uint64_t v = rd_le(r, n);
    if (v < (n == 2 ? 0xfd : n == 4 ? 0x10000 : 0x100000000ull) || v > 0x02000000) r->bad = true;
    return r->bad ? 0 : v;
}

/* Where the parts of a serialized transaction are, as offsets into it. body: the input count (4, or 6 after the marker and flag of
 * a segwit transaction); outs: the output count; wit: the witness (equal to lock when there is none); lock: the lock time. */
typedef struct {
    size_t body, outs, wit, lock;
    uint64_t n_out;
} txmap;

/* Walks raw, which must be exactly one transaction, and maps it. With tx not NULL its fields go there too, within its capacities. */
static bool tx_walk(const uint8_t *raw, size_t len, txmap *m, btc_tx *tx)
{
    rd r = {raw, raw + len, false};
    const uint32_t version = (uint32_t)rd_le(&r, 4);
    /* After the version, a zero where the input count would be is the marker of the segwit serialization (a transaction has inputs);
     * the flag that follows must be 1, the only one defined. */
    const bool seg = len > 5 && raw[4] == 0;
    if (seg && raw[5] != 1) return false;
    if (seg) r.p += 2;
    m->body = (size_t)(r.p - raw);
    const uint64_t n_in = rd_size(&r);
    if (!n_in || (tx && n_in > BTC_MAX_IN)) return false;
    for (uint64_t k = 0; k < n_in && !r.bad; k++) {
        const uint8_t *op = rd_take(&r, 36);
        const uint64_t sl = rd_size(&r);
        const uint8_t *sc = rd_take(&r, sl);
        const uint32_t seq = (uint32_t)rd_le(&r, 4);
        if (!tx || r.bad) continue;
        if (sl > BTC_SCRIPTSIG_MAX) return false;
        btc_in *in = &tx->in[k];
        memcpy(in->prev_txid, op, 32);
        in->prev_vout = (uint32_t)get_le(op + 32, 4);
        in->sequence = seq;
        in->script_len = (uint8_t)sl;
        if (sl) memcpy(in->script, sc, sl);
    }
    m->outs = (size_t)(r.p - raw);
    const uint64_t n_out = rd_size(&r);
    if (!n_out || (tx && n_out > BTC_MAX_OUT)) return false;
    uint64_t total = 0;
    for (uint64_t k = 0; k < n_out && !r.bad; k++) {
        const uint64_t value = rd_le(&r, 8);
        const uint64_t sl = rd_size(&r);
        const uint8_t *sc = rd_take(&r, sl);
        if (value > BTC_MAX_MONEY || (total += value) > BTC_MAX_MONEY) return false;   /* as the node checks them */
        if (!tx || r.bad) continue;
        if (sl > BTC_SPK_MAX) return false;
        btc_out *out = &tx->out[k];
        out->value = value;
        out->script_len = (uint8_t)sl;
        if (sl) memcpy(out->script, sc, sl);
    }
    m->wit = (size_t)(r.p - raw);
    if (seg) {
        bool any = false;
        for (uint64_t k = 0; k < n_in && !r.bad; k++) {
            const uint64_t items = rd_size(&r);
            any |= items != 0;
            for (uint64_t j = 0; j < items && !r.bad; j++) rd_take(&r, rd_size(&r));
        }
        if (!any) return false;   /* the flag with no witness at all: the node refuses it ("superfluous witness record") */
    }
    m->lock = (size_t)(r.p - raw);
    const uint32_t lock = (uint32_t)rd_le(&r, 4);
    if (r.bad || r.p != r.end) return false;
    m->n_out = n_out;
    if (tx) {
        tx->version = version;
        tx->locktime = lock;
        tx->n_in = (size_t)n_in;
        tx->n_out = (size_t)n_out;
    }
    return true;
}

/* the txid of a mapped transaction: the hash of its legacy serialization, which for a segwit one is the version, the inputs and
 * outputs and the lock time without the marker, the flag and the witness */
static bool txid_of(const uint8_t *raw, size_t len, const txmap *m, uint8_t txid[32])
{
    if (m->body == 4) {
        pz_sha256d(raw, len, txid);
        return true;
    }
    const size_t mid = m->wit - m->body, n = 4 + mid + 4;
    uint8_t *b = malloc(n);
    if (!b) return false;
    memcpy(b, raw, 4);
    memcpy(b + 4, raw + m->body, mid);
    memcpy(b + 4 + mid, raw + m->lock, 4);
    pz_sha256d(b, n, txid);
    free(b);
    return true;
}

bool btc_txid(const uint8_t *raw, size_t len, uint8_t txid[32])
{
    txmap m;
    return raw && tx_walk(raw, len, &m, NULL) && txid_of(raw, len, &m, txid);
}

bool btc_prevout(const uint8_t *raw, size_t len, const uint8_t txid[32], uint32_t vout, uint64_t *value, const uint8_t **spk,
                 size_t *spk_len)
{
    txmap m;
    uint8_t h[32];
    if (!raw || !tx_walk(raw, len, &m, NULL) || vout >= m.n_out || !txid_of(raw, len, &m, h) || memcmp(h, txid, 32) != 0) return false;
    rd r = {raw + m.outs, raw + m.wit, false};
    rd_size(&r);
    for (uint32_t k = 0;; k++) {   /* the walk checked every output, so these reads cannot fail */
        const uint64_t v = rd_le(&r, 8);
        const uint64_t sl = rd_size(&r);
        const uint8_t *s = rd_take(&r, sl);
        if (k == vout) {
            *value = v;
            *spk = s;
            *spk_len = (size_t)sl;
            return !r.bad;
        }
    }
}

bool btc_tx_parse(btc_tx *tx, const uint8_t *raw, size_t len)
{
    txmap m;
    if (raw && tx_walk(raw, len, &m, tx)) return true;
    memset(tx, 0, sizeof *tx);
    return false;
}

/* ---- writing ---- */

void btc_tx_init(btc_tx *tx)
{
    memset(tx, 0, sizeof *tx);
    tx->version = 2;
}

bool btc_tx_add_in(btc_tx *tx, const uint8_t prev_txid[32], uint32_t prev_vout, uint32_t sequence)
{
    if (tx->n_in >= BTC_MAX_IN) return false;
    btc_in *in = &tx->in[tx->n_in++];
    memset(in, 0, sizeof *in);
    memcpy(in->prev_txid, prev_txid, 32);
    in->prev_vout = prev_vout;
    in->sequence = sequence;
    return true;
}

bool btc_tx_add_out(btc_tx *tx, uint64_t value, const uint8_t *spk, size_t len)
{
    if (tx->n_out >= BTC_MAX_OUT || len > BTC_SPK_MAX || (len && !spk) || value > BTC_MAX_MONEY) return false;
    btc_out *out = &tx->out[tx->n_out++];
    memset(out, 0, sizeof *out);
    out->value = value;
    out->script_len = (uint8_t)len;
    if (len) memcpy(out->script, spk, len);
    return true;
}

/* a btc_tx that can be serialized and parsed back: at least one input and one output (with none, the serialization would read as
 * the marker of a segwit one), and every count and length within the capacities */
static bool tx_ok(const btc_tx *tx)
{
    if (!tx || !tx->n_in || tx->n_in > BTC_MAX_IN || !tx->n_out || tx->n_out > BTC_MAX_OUT) return false;
    for (size_t k = 0; k < tx->n_in; k++)
        if (tx->in[k].script_len > BTC_SCRIPTSIG_MAX) return false;
    for (size_t k = 0; k < tx->n_out; k++)
        if (tx->out[k].script_len > BTC_SPK_MAX) return false;
    return true;
}

#define NO_INPUT SIZE_MAX

/* The legacy serialization. With sig_in an input, it is the one its signature hash covers: the scriptSig of that input replaced by
 * code, those of the others empty. */
static size_t tx_write(const btc_tx *tx, uint8_t *out, size_t sig_in, const uint8_t *code, size_t code_len)
{
    uint8_t *o = put_le32(out, tx->version);
    o = put_size(o, tx->n_in);
    for (size_t k = 0; k < tx->n_in; k++) {
        const btc_in *in = &tx->in[k];
        const uint8_t *s = in->script;
        size_t sl = in->script_len;
        if (sig_in != NO_INPUT) {
            s = code;
            sl = k == sig_in ? code_len : 0;
        }
        memcpy(o, in->prev_txid, 32);
        o = put_le32(o + 32, in->prev_vout);
        o = put_size(o, sl);
        if (sl) memcpy(o, s, sl);
        o = put_le32(o + sl, in->sequence);
    }
    o = put_size(o, tx->n_out);
    for (size_t k = 0; k < tx->n_out; k++) {
        const btc_out *ot = &tx->out[k];
        o = put_le64(o, ot->value);
        o = put_size(o, ot->script_len);
        if (ot->script_len) memcpy(o, ot->script, ot->script_len);
        o += ot->script_len;
    }
    o = put_le32(o, tx->locktime);
    return (size_t)(o - out);
}

size_t btc_tx_size(const btc_tx *tx)
{
    if (!tx_ok(tx)) return 0;
    size_t n = 4 + size_len(tx->n_in) + size_len(tx->n_out) + 4;
    for (size_t k = 0; k < tx->n_in; k++) n += 36 + size_len(tx->in[k].script_len) + tx->in[k].script_len + 4;
    for (size_t k = 0; k < tx->n_out; k++) n += 8 + size_len(tx->out[k].script_len) + tx->out[k].script_len;
    return n;
}

size_t btc_tx_serialize(const btc_tx *tx, uint8_t *out, size_t cap)
{
    const size_t n = btc_tx_size(tx);
    if (!n || n > cap || !out) return 0;
    return tx_write(tx, out, NO_INPUT, NULL, 0);
}

bool btc_tx_txid(const btc_tx *tx, uint8_t txid[32])
{
    uint8_t b[BTC_TX_MAX_SIZE];
    const size_t n = btc_tx_serialize(tx, b, sizeof b);
    if (!n) return false;
    pz_sha256d(b, n, txid);
    return true;
}

/* ---- signatures ---- */

#define SIGHASH_ALL 1

bool btc_sighash_legacy(const btc_tx *tx, size_t i, const uint8_t *script_code, size_t code_len, uint8_t digest[32])
{
    if (!tx_ok(tx) || i >= tx->n_in || code_len > BTC_SCRIPTCODE_MAX || (code_len && !script_code)) return false;
    uint8_t b[BTC_TX_MAX_SIZE + BTC_SCRIPTCODE_MAX + 4];   /* the scriptSigs are left out, script_code and the hashtype come in */
    const size_t n = tx_write(tx, b, i, script_code, code_len);
    put_le32(b + n, SIGHASH_ALL);
    pz_sha256d(b, n + 4, digest);
    return true;
}

size_t btc_der_encode(const uint8_t r[32], const uint8_t s[32], uint8_t der[BTC_DER_MAX])
{
    static const uint8_t zero[32];
    uint8_t rs[64];
    secp256k1_ecdsa_signature sig, low;
    memcpy(rs, r, 32);
    memcpy(rs + 32, s, 32);
    /* parse_compact refuses values not below n, and lets zero through: zero is refused here */
    if (!memcmp(r, zero, 32) || !memcmp(s, zero, 32) || !secp256k1_ecdsa_signature_parse_compact(ctx(), &sig, rs)) return 0;
    secp256k1_ecdsa_signature_normalize(ctx(), &low, &sig);
    size_t n = BTC_DER_MAX;
    return secp256k1_ecdsa_signature_serialize_der(ctx(), der, &n, &low) ? n : 0;
}

bool btc_put_sig_p2pkh(btc_tx *tx, size_t i, const uint8_t r[32], const uint8_t s[32], const uint8_t pub[33])
{
    uint8_t der[BTC_DER_MAX];
    if (!tx_ok(tx) || i >= tx->n_in || (pub[0] != 2 && pub[0] != 3)) return false;
    const size_t n = btc_der_encode(r, s, der);
    if (!n) return false;
    btc_in *in = &tx->in[i];
    uint8_t *o = in->script;
    *o++ = (uint8_t)(n + 1);   /* a direct push: the signature and its hashtype */
    memcpy(o, der, n);
    o += n;
    *o++ = SIGHASH_ALL;
    *o++ = 33;
    memcpy(o, pub, 33);
    in->script_len = (uint8_t)(o + 33 - in->script);
    return true;
}

bool btc_key_script(const uint8_t seckey[32], uint8_t spk[BTC_P2PKH_LEN])
{
    secp256k1_pubkey pk;
    uint8_t pub[33], h160[20];
    size_t pl = sizeof pub;
    if (!secp256k1_ec_pubkey_create(ctx(), &pk, seckey)) return false;
    secp256k1_ec_pubkey_serialize(ctx(), pub, &pl, &pk, SECP256K1_EC_COMPRESSED);
    pz_hash160(pub, sizeof pub, h160);
    btc_script_p2pkh(h160, spk);
    return true;
}

bool btc_sign_p2pkh(btc_tx *tx, size_t i, const uint8_t seckey[32])
{
    const secp256k1_context *c = ctx();
    secp256k1_pubkey pk;
    secp256k1_ecdsa_signature sig;
    uint8_t pub[33], h160[20], code[BTC_P2PKH_LEN], digest[32], rs[64];
    size_t pl = sizeof pub;
    if (!tx_ok(tx) || i >= tx->n_in || !secp256k1_ec_pubkey_create(c, &pk, seckey)) return false;
    secp256k1_ec_pubkey_serialize(c, pub, &pl, &pk, SECP256K1_EC_COMPRESSED);
    pz_hash160(pub, sizeof pub, h160);
    btc_script_p2pkh(h160, code);
    if (!btc_sighash_legacy(tx, i, code, sizeof code, digest)) return false;
    /* NULL nonce function: the default, RFC 6979. The signature comes out with low s. */
    if (!secp256k1_ecdsa_sign(c, &sig, digest, seckey, NULL, NULL)) return false;
    secp256k1_ecdsa_signature_serialize_compact(c, rs, &sig);
    return btc_put_sig_p2pkh(tx, i, rs, rs + 32, pub);
}

bool btc_ecdsa_verify(const uint8_t digest[32], const uint8_t *der, size_t der_len, const uint8_t *pub, size_t pub_len)
{
    const secp256k1_context *c = ctx();
    secp256k1_ecdsa_signature sig;
    secp256k1_pubkey pk;
    if (!der || !pub || der_len < 8 || der_len > BTC_DER_MAX) return false;
    /* the forms the node takes as standard: libsecp256k1 would also parse the hybrid ones (06, 07) */
    if (!(pub_len == 33 && (pub[0] == 2 || pub[0] == 3)) && !(pub_len == 65 && pub[0] == 4)) return false;
    if (!secp256k1_ecdsa_signature_parse_der(c, &sig, der, der_len) || !secp256k1_ec_pubkey_parse(c, &pk, pub, pub_len)) return false;
    return secp256k1_ecdsa_verify(c, &sig, digest, &pk) == 1;   /* which also refuses a high s */
}

bool btc_verify_p2pkh(const btc_tx *tx, size_t i, const uint8_t *prev_spk, size_t spk_len)
{
    uint8_t h160[20], kh[20], digest[32];
    if (!tx_ok(tx) || i >= tx->n_in || !btc_script_is_p2pkh(prev_spk, spk_len, h160)) return false;
    /* exactly <push sig+hashtype> <push key>, each with the direct push opcode, which for these sizes is the minimal one */
    const btc_in *in = &tx->in[i];
    const uint8_t *s = in->script;
    const size_t n = in->script_len;
    if (n < 1) return false;
    const size_t sl = s[0];
    if (sl < 9 || sl > BTC_DER_MAX + 1 || n < 1 + sl + 1) return false;
    const size_t kl = s[1 + sl];
    if ((kl != 33 && kl != 65) || n != 1 + sl + 1 + kl) return false;
    const uint8_t *sig = s + 1, *key = s + 2 + sl;
    if (sig[sl - 1] != SIGHASH_ALL) return false;
    pz_hash160(key, kl, kh);
    if (memcmp(kh, h160, 20) != 0) return false;
    return btc_sighash_legacy(tx, i, prev_spk, spk_len, digest) && btc_ecdsa_verify(digest, sig, sl - 1, key, kl);
}

/* ---- size and fee ---- */

size_t btc_vsize_p2pkh(size_t n_in, const btc_out *out, size_t n_out)
{
    size_t n = 4 + size_len(n_in) + n_in * BTC_P2PKH_IN_SIZE + size_len(n_out) + 4;
    for (size_t k = 0; k < n_out; k++) n += 8 + size_len(out[k].script_len) + out[k].script_len;
    return n;
}

uint64_t btc_fee(size_t vsize, uint64_t sat_per_kvb)
{
    return (uint64_t)(((pz_u128)vsize * sat_per_kvb + 999) / 1000);
}
