/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* fuzz_peer.c - a mutation fuzzer of the readers of hostile input in pzpeer, for a given number of seconds (make peer-fuzz, best under
 * the sanitizers). Seeds: replies of bitcoind and results of pzsign, transactions of the main chain and of the vectors, HTTP replies,
 * records, claims of blocks and network files. Mutations: bytes changed, inserted, removed, cut, repeated, spliced from another seed,
 * and tokens of each grammar (escapes, surrogates, long numbers, CompactSize lengths at their limits, chunk sizes, keys of a network
 * file). Besides staying in their buffers, the readers must agree:
 *   JSON: a document that parses is written again with the writer and parses to the same tree;
 *   transactions: what btc_tx_parse takes has the txid of btc_txid and serializes back to its legacy form; btc_prevout gives every
 *     output inside the bytes; a mutated signed input that btc_verify_p2pkh accepts is the original one;
 *   HTTP: a reply read in random pieces gives what it gives read at once (the reader of btc_rpc.c is included here for that);
 *   records and claims (what the table takes from the network): what decodes encodes back to the same bytes, and the checks take it;
 *   network files: one that parses has the same fingerprint and options with "\r\n" at the ends of its lines and a comment after them.
 * Usage: fuzz_peer [SECONDS [SEED]]. Exit status 1 on the first disagreement, with the input in hex. */
#include "../../src/peer/btc_rpc.c"
#include "block.h"
#include "btc_vectors.h"
#include "keys.h"
#include "netfile.h"
#include "peertest.h"
#include "record.h"
#include <time.h>

#define NV(a) (sizeof (a) / sizeof (a)[0])

static uint64_t g_x;

static uint64_t rnd(void)
{
    g_x ^= g_x << 13;
    g_x ^= g_x >> 7;
    g_x ^= g_x << 17;
    return g_x;
}

static size_t below(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static void dump(const char *what, const uint8_t *p, size_t n)
{
    printf("FAIL %s, input of %zu bytes:\n", what, n);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
    exit(1);
}

/* ---- mutations ---- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf;

static void put(buf *b, size_t at, const void *s, size_t n)
{
    if (b->len + n > b->cap) return;
    memmove(b->p + at + n, b->p + at, b->len - at);
    memcpy(b->p + at, s, n);
    b->len += n;
}

typedef struct {
    const char *s;
    size_t n;
} token;
#define T(s) {s, sizeof s - 1}

/* one mutation of b, with the tokens of its grammar and the other seeds to splice from (never b itself) */
static void mutate(buf *b, const token *tokens, size_t nt, const buf *seeds, size_t ns)
{
    const size_t at = below(b->len + 1);
    switch (rnd() % 9) {
    case 0: case 1:   /* a byte changed */
        if (b->len) b->p[below(b->len)] = (uint8_t)rnd();
        break;
    case 2:           /* a bit flipped */
        if (b->len) b->p[below(b->len)] ^= (uint8_t)(1u << (rnd() % 8));
        break;
    case 3: {         /* a byte inserted */
        const uint8_t c = (uint8_t)rnd();
        put(b, at, &c, 1);
        break;
    }
    case 4: {         /* a range removed */
        const size_t n = below(b->len - at < 16 ? b->len - at + 1 : 17);
        memmove(b->p + at, b->p + at + n, b->len - at - n);
        b->len -= n;
        break;
    }
    case 5:           /* cut */
        b->len = at;
        break;
    case 6: {         /* a range repeated */
        if (!b->len) break;
        const size_t from = below(b->len), n = 1 + below(b->len - from < 64 ? b->len - from : 64);
        uint8_t tmp[64];
        memcpy(tmp, b->p + from, n);
        put(b, below(b->len + 1), tmp, n);
        break;
    }
    case 7: {         /* a piece of another seed */
        const buf *o = &seeds[below(ns)];
        if (!o->len) break;
        const size_t from = below(o->len), n = 1 + below(o->len - from < 256 ? o->len - from : 256);
        put(b, at, o->p + from, n);
        break;
    }
    default: {        /* a token */
        const token *t = &tokens[below(nt)];
        put(b, at, t->s, t->n);
        break;
    }
    }
}

static void load(buf *b, const void *p, size_t n, size_t cap)
{
    b->cap = cap > n ? cap : n;
    b->p = malloc(b->cap);
    memcpy(b->p, p, n);
    b->len = n;
}

/* ---- JSON ---- */

static const token JSON_TOKENS[] = {
    T("{"), T("}"), T("["), T("]"), T("\""), T(","), T(":"), T("\\"), T("\\u"), T("\\u0000"), T("\\ud800"), T("\\udc00"),
    T("\\ud83d\\ude00"), T("\\uDBFF\\uDFFF"), T("\\n"), T("\\\""), T("true"), T("false"), T("null"), T("-"), T("0"), T("-0"),
    T("1e999999"), T("1E-999999"), T("0.00000001"), T("21000000.00000001"), T("18446744073709551615"), T("18446744073709551616"),
    T("-9223372036854775808"), T("-9223372036854775809"), T("1.5e-8"), T("00"), T(" "), T("\t"), T("\r\n"), T("\xc3\xa9"),
    T("\xed\xa0\x80"), T("\xf4\x90\x80\x80"), T("\xc0\x80"), T("\xe2\x82"), T("\xff"), T("\x7f"), T("\x00"), T("{\"a\":"),
    T("[[[[[[[[[[[[[[[["), T("]]]]]]]]]]]]]]]]"),
};

static const char *const JSON_SEEDS[] = {
    "{\"result\":{\"success\":true,\"txouts\":185000000,\"height\":870000,\"bestblock\":\"00000000000000000001\",\"unspents\":[{\"txid\":"
    "\"f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16\",\"vout\":1,\"scriptPubKey\":\"76a914751e76e8199196d454941c45d1b3a3"
    "23f1433bd688ac\",\"desc\":\"addr(1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH)#x\",\"amount\":0.00012345,\"coinbase\":false,\"height\":869000,"
    "\"blockhash\":\"0000\"}],\"total_amount\":0.00012345},\"error\":null,\"id\":7}",
    "{\"result\":null,\"error\":{\"code\":-26,\"message\":\"min relay fee not met, 100 < 191\"},\"id\":3}",
    "{\"result\":{\"feerate\":0.00012,\"blocks\":2},\"error\":null,\"id\":1}",
    "{\"result\":{\"chain\":\"main\",\"blocks\":869990,\"headers\":870000,\"initialblockdownload\":false,\"pruned\":true,\"warnings\":[]},"
    "\"error\":null,\"id\":2}",
    "{\"sigs\":[{\"r\":\"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\",\"s\":\"0000000000000000000000000000000000"
    "000000000000000000000000000001\"}]}",
    "{\"error\":\"the partner sent a bad message \\u00e9\\ud83d\\ude00\",\"blame\":[1,0]}",
    "[1.5e-3,-0,\"\\/\",{},[],{\"\":[null,true,false]},-12345678901234567890,1E+2]",
};

/* the tree written again, every value with the writer; *exact false when a key has a NUL, which jw_key (a C string) cannot write */
static void rewrite(json_w *w, const json_val *v, bool *exact)
{
    switch (v->type) {
    case JSON_NULL: jw_null(w); break;
    case JSON_FALSE: jw_bool(w, false); break;
    case JSON_TRUE: jw_bool(w, true); break;
    case JSON_NUMBER: jw_raw(w, v->s, v->n); break;
    case JSON_STRING: jw_strn(w, v->s, v->n); break;
    case JSON_ARRAY: {
        jw_arr(w);
        const json_val *e = v + 1;
        for (uint32_t i = 0; i < v->n; i++, e = json_next(e)) rewrite(w, e, exact);
        jw_end(w);
        break;
    }
    case JSON_OBJECT: {
        jw_obj(w);
        const json_val *k = v + 1;
        for (uint32_t i = 0; i < v->n; i++) {
            *exact &= !memchr(k->s, 0, k->n);
            jw_key(w, k->s);   /* the text up to its NUL */
            rewrite(w, k + 1, exact);
            k = json_next(k + 1);
        }
        jw_end(w);
        break;
    }
    }
}

/* whether the trees are the same: the types, the counts, the bytes of the strings and the text of the numbers */
static bool same_tree(const json_doc *a, const json_doc *b)
{
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++) {
        const json_val *x = &a->v[i], *y = &b->v[i];
        if (x->type != y->type || x->n != y->n || x->span != y->span) return false;
        if ((x->type == JSON_STRING || x->type == JSON_NUMBER) && memcmp(x->s, y->s, x->n)) return false;
    }
    return true;
}

static bool spans_ok(const json_doc *d)
{
    if (!d->count || d->v[0].span != d->count) return false;
    for (size_t i = 0; i < d->count; i++) {
        const json_val *v = &d->v[i];
        if (!v->span || i + v->span > d->count) return false;
        if (v->type == JSON_ARRAY || v->type == JSON_OBJECT) {
            size_t inner = 1;
            const json_val *e = v + 1;
            for (uint32_t k = 0; k < v->n * (v->type == JSON_OBJECT ? 2u : 1u); k++) {
                if (e >= d->v + d->count) return false;
                inner += e->span;
                e = json_next(e);
            }
            if (inner != v->span) return false;
        } else if (v->span != 1) return false;
    }
    return true;
}

static unsigned long g_json_parsed, g_json_runs;

static void fuzz_json(buf *seeds, size_t ns)
{
    buf b;
    const buf *s = &seeds[below(ns)];
    load(&b, s->p, s->len, s->len + 1024);
    for (int m = 1 + (int)below(6); m; m--) mutate(&b, JSON_TOKENS, NV(JSON_TOKENS), seeds, ns);
    char *t = malloc(b.len ? b.len : 1);   /* exactly the bytes: a read past them is caught by the sanitizer */
    memcpy(t, b.p, b.len);
    json_doc d;
    g_json_runs++;
    if (json_parse(&d, t, b.len)) {
        g_json_parsed++;
        if (!spans_ok(&d)) dump("json: spans", b.p, b.len);
        for (size_t i = 0; i < d.count; i++) {   /* the typed reads take whatever came */
            uint64_t u;
            int64_t sv;
            bool bo;
            if (json_sats(&d.v[i], &u) && u > JSON_MAX_SATS) dump("json: sats above the limit", b.p, b.len);
            (void)json_u64(&d.v[i], &u), (void)json_i64(&d.v[i], &sv), (void)json_bool(&d.v[i], &bo), (void)json_str(&d.v[i]);
        }
        json_w w;
        jw_init(&w);
        bool exact = true;
        rewrite(&w, d.v, &exact);
        size_t n;
        const char *again = jw_done(&w, &n);
        json_doc d2 = {0};
        if (!again || !json_parse(&d2, again, n)) dump("json: the writer's text does not parse", b.p, b.len);
        if (exact && !same_tree(&d, &d2)) dump("json: written again, another tree", b.p, b.len);
        json_free(&d2);
        jw_free(&w);
        json_free(&d);
    }
    free(t);
    free(b.p);
}

/* ---- transactions ---- */

static const token TX_TOKENS[] = {
    T("\xfd"), T("\xfe"), T("\xff"), T("\xfd\xfc\x00"), T("\xfd\xfd\x00"), T("\xfe\xff\xff\x00\x00"), T("\xfe\x00\x00\x00\x02"),
    T("\xfe\x01\x00\x00\x02"), T("\xff\x00\x00\x00\x00\x01\x00\x00\x00"), T("\x00\x01"), T("\x00\x02"), T("\x00"), T("\x01"), T("\x10"),
    T("\x11"), T("\x8c"), T("\x8d"), T("\x53"), T("\x54"), T("\x19\x76\xa9\x14"), T("\x88\xac"), T("\x00\x40\x07\x5a\xf0\x75\x07\x00"),
    T("\x01\x40\x07\x5a\xf0\x75\x07\x00"), T("\xff\xff\xff\xff\xff\xff\xff\xff"), T("\xff\xff\xff\xff"), T("\x00\x00\x00\x00"),
};

static unsigned long g_tx_parsed, g_tx_hashed, g_tx_runs;

static void fuzz_tx(buf *seeds, size_t ns)
{
    buf b;
    const buf *s = &seeds[below(ns)];
    load(&b, s->p, s->len, s->len + 512);
    for (int m = 1 + (int)below(4); m; m--) mutate(&b, TX_TOKENS, NV(TX_TOKENS), seeds, ns);
    uint8_t *raw = malloc(b.len ? b.len : 1);
    memcpy(raw, b.p, b.len);
    btc_tx tx, back;
    uint8_t h1[32], h2[32];
    memset(&tx, 0, sizeof tx);   /* the parts a parse leaves alone, the same in both for the comparison */
    memset(&back, 0, sizeof back);
    g_tx_runs++;
    const bool parsed = btc_tx_parse(&tx, raw, b.len), hashed = btc_txid(raw, b.len, h1);
    if (parsed) {
        g_tx_parsed++;
        uint8_t out[BTC_TX_MAX_SIZE];
        const size_t n = btc_tx_serialize(&tx, out, sizeof out);
        if (!hashed || !btc_tx_txid(&tx, h2) || memcmp(h1, h2, 32)) dump("tx: btc_tx_parse and btc_txid disagree", b.p, b.len);
        if (!n || !btc_tx_parse(&back, out, n) || memcmp(&back, &tx, sizeof tx)) dump("tx: serialized, not the same", b.p, b.len);
        if (!(b.len > 5 && raw[4] == 0) && (n != b.len || memcmp(out, raw, n))) dump("tx: legacy, not the same bytes", b.p, b.len);
        for (size_t i = 0; i < tx.n_in && i < tx.n_out; i++)   /* whatever scriptSigs came, as spends of the outputs: none is valid */
            if (btc_verify_p2pkh(&tx, i, tx.out[i].script, tx.out[i].script_len)) dump("tx: a spend verified", b.p, b.len);
    }
    if (hashed) {
        g_tx_hashed++;
        for (uint32_t v = 0;; v++) {
            uint64_t value;
            const uint8_t *spk;
            size_t sl;
            if (!btc_prevout(raw, b.len, h1, v, &value, &spk, &sl)) {
                if (!v) dump("tx: no output 0", b.p, b.len);
                break;
            }
            if (spk < raw || spk + sl > raw + b.len || value > BTC_MAX_MONEY) dump("tx: an output outside", b.p, b.len);
            if (parsed && (v >= tx.n_out || tx.out[v].value != value || tx.out[v].script_len != sl)) dump("tx: btc_prevout", b.p, b.len);
        }
    }
    free(raw);
    free(b.p);
}

/* A signed spend: a mutation of its scriptSig that verifies must be the scriptSig itself (strict DER, low s, one hashtype, minimal
 * pushes: no other encoding of the same signature is taken). */
static void fuzz_sig(const btc_tx *signed_tx, const uint8_t *spk)
{
    btc_tx tx = *signed_tx;
    btc_in *in = &tx.in[0];
    buf b;
    load(&b, in->script, in->script_len, BTC_SCRIPTSIG_MAX + 64);
    static const token toks[] = {T("\x30"), T("\x02"), T("\x00"), T("\x01"), T("\x81"), T("\x21"), T("\x41"), T("\x47"), T("\x48"),
                                 T("\x49"), T("\x4c"), T("\x80"), T("\x02\x21\x00"), T("\x02\x20")};
    buf orig;   /* to splice from */
    load(&orig, in->script, in->script_len, 0);
    for (int m = 1 + (int)below(3); m; m--) mutate(&b, toks, NV(toks), &orig, 1);
    free(orig.p);
    if (b.len <= BTC_SCRIPTSIG_MAX) {
        memcpy(in->script, b.p, b.len);
        in->script_len = (uint8_t)b.len;
        if (btc_verify_p2pkh(&tx, 0, spk, BTC_P2PKH_LEN) &&
            (b.len != signed_tx->in[0].script_len || memcmp(b.p, signed_tx->in[0].script, b.len)))
            dump("sig: another scriptSig verified", b.p, b.len);
    }
    free(b.p);
}

/* ---- HTTP ---- */

static const token HTTP_TOKENS[] = {
    T("\r\n"), T("\r\n\r\n"), T("\n"), T("\r"), T(":"), T(" "), T("Content-Length: "), T("Transfer-Encoding: chunked\r\n"),
    T("0\r\n\r\n"), T("ffffff\r\n"), T("1;x\r\n"), T("10000000000\r\n"), T("0000000000000001\r\n"), T("HTTP/1.1 200 OK\r\n"),
    T("HTTP/1.0 "), T("999"), T("Content-Length: 99999999999\r\n"), T("\x00"), T("1\r\nx\r\n"),
};

static const char *const HTTP_SEEDS[] = {
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 37\r\n\r\n{\"result\":42,\"error\":null,\"id\":1}\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n7\r\n{\"resul\r\n1A;e=1\r\nt\":42,\"error\":null,\"id\":1}\r\n0\r\nX-T: 1\r\n\r\n",
    "HTTP/1.0 500\r\nServer: x\r\n\r\n{\"result\":null,\"error\":{\"code\":-5,\"message\":\"no\"},\"id\":2}",
    "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nWWW-Authenticate: Basic realm=\"jsonrpc\"\r\n\r\n",
};

typedef struct {
    int done;          /* the last result of body_done */
    int status;
    size_t body;
    char *data;
} http_out;

/* the reply fed to the reader in pieces of the given sizes (0: all at once), with the end of the connection after it */
static http_out read_reply(const uint8_t *p, size_t n, bool pieces)
{
    reply r = {0};
    r.cap = n + 1;
    r.p = malloc(r.cap);
    int d = 0;
    size_t at = 0;
    for (;;) {
        const size_t k = pieces ? below(n - at < 64 ? n - at + 1 : 65) : n - at;
        memcpy(r.p + r.len, p + at, k);
        at += k;
        r.len += k;
        d = body_done(&r, false);
        if (d) break;
        if (at == n) {
            d = body_done(&r, true);
            break;
        }
    }
    http_out o = {d, r.status, r.body, NULL};
    if (d > 0) {
        if (r.head + r.body > r.len) dump("http: the body is outside the reply", p, n);
        o.data = malloc(r.body ? r.body : 1);
        memcpy(o.data, r.p + r.head, r.body);
    }
    free(r.p);
    return o;
}

static unsigned long g_http_done, g_http_runs;

static void fuzz_http(buf *seeds, size_t ns)
{
    buf b;
    const buf *s = &seeds[below(ns)];
    load(&b, s->p, s->len, s->len + 512);
    for (int m = (int)below(5); m; m--) mutate(&b, HTTP_TOKENS, NV(HTTP_TOKENS), seeds, ns);
    g_http_runs++;
    /* the reader keeps its place between reads: in pieces it must end as it ends with all the bytes at once */
    http_out a = read_reply(b.p, b.len, true), c = read_reply(b.p, b.len, false);
    if (a.done != c.done || (a.done > 0 && (a.status != c.status || a.body != c.body || memcmp(a.data, c.data, a.body))))
        dump("http: in pieces and at once differ", b.p, b.len);
    g_http_done += a.done > 0;
    free(a.data);
    free(c.data);
    free(b.p);
}

/* ---- records, claims, network files ---- */

static const token WIRE_TOKENS[] = {T("\x00"), T("\x01"), T("\xff"), T("\x80"), T("\x02"), T("\x40"), T("\x41"), T("\x7f"), T("\x00\x00\x00\x00")};

static record_net g_net;
static block_net g_blocks;
static unsigned long g_rec_taken, g_rec_runs, g_claim_taken, g_claim_runs, g_net_parsed, g_net_runs;

/* a record (RECORD_LEN bytes, or the RECORD_WIRE of a STORE): what decodes is those bytes again; the checks take it */
static void fuzz_record(buf *seeds, size_t ns)
{
    buf b;
    const buf *s = &seeds[below(ns)];
    load(&b, s->p, s->len, s->len + 64);
    for (int m = 1 + (int)below(3); m; m--) mutate(&b, WIRE_TOKENS, NV(WIRE_TOKENS), seeds, ns);
    uint8_t in[RECORD_LEN], out[RECORD_LEN];
    record r;
    g_rec_runs++;
    if (b.len == RECORD_LEN || b.len == RECORD_WIRE) {
        memcpy(in, b.p, b.len);
        static const uint8_t SENDER[32] = {2};                 /* the owner of the records of a STORE */
        const bool ok = b.len == RECORD_LEN ? record_decode(in, &r) : record_wire_decode(in, SENDER, &r);
        if (ok) {
            g_rec_taken++;
            record_encode(&r, out);
            if (memcmp(in, out, b.len)) dump("record: decoded, not the same bytes", b.p, b.len);
            (void)record_check(keys_ctx(), &g_net, &r);
        }
    }
    free(b.p);
}

/* a claim of a block: what decodes, of exactly its length, is its bytes again; the checks take it */
static void fuzz_claim(buf *seeds, size_t ns)
{
    buf b;
    const buf *s = &seeds[below(ns)];
    load(&b, s->p, s->len, s->len + 256);
    for (int m = 1 + (int)below(4); m; m--) mutate(&b, WIRE_TOKENS, NV(WIRE_TOKENS), seeds, ns);
    uint8_t *in = malloc(b.len ? b.len : 1), out[BLOCK_CLAIM_MAX];
    memcpy(in, b.p, b.len);
    block_claim c;
    g_claim_runs++;
    if (block_claim_decode(in, b.len, &c)) {
        g_claim_taken++;
        if (block_claim_encode(&c, out) != b.len || memcmp(in, out, b.len)) dump("claim: decoded, not the same bytes", b.p, b.len);
        (void)block_claim_check(keys_ctx(), &g_blocks, &c);
        (void)block_claims_keys(&c, 1);
    }
    free(in);
    free(b.p);
}

static const token NET_TOKENS[] = {
    T("\n"), T("="), T("#"), T(" "), T("\t"), T("\r"), T(":"), T("net="), T("puzzle="), T("pubkey="), T("range="), T("jumpseed="),
    T("schema=net-06"), T("dpbits="), T("chain=main"), T("chain=regtest"), T("address="), T("blockbits="), T("entry=127.0.0.1:1"),
    T("prize="), T("checkpoint="), T("ffffffffffffffffffffffffffffffff"), T("0"), T("60"), T("256"), T("\x00"),
};

static const char *const NET_SEEDS[] = {
    "# a test network\nnet=t1\npuzzle=0\npubkey=0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\nrange=1000:ffff\n"
    "jumpseed=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\nschema=net-06\ndpbits=8\nchain=regtest\nentry=h:1\n",
    "net=lin\npuzzle=0\naddress=mmF9pY4RGQVXonKpcvvYPxkEgs42C3iSRz\nrange=1000:ffff\nblockbits=36\nchain=regtest\nelectrum=a:1\n",
};

/* a network file: one that parses parses the same with "\r\n" at the ends of its lines (when they still fit) and a comment after them */
static void fuzz_netfile(buf *seeds, size_t ns)
{
    buf b;
    const buf *s = &seeds[below(ns)];
    load(&b, s->p, s->len, s->len + 256);
    for (int m = 1 + (int)below(4); m; m--) mutate(&b, NET_TOKENS, NV(NET_TOKENS), seeds, ns);
    char *t = malloc(b.len + 1), *u = malloc(2 * b.len + 16), err[300];   /* u: t with "\r\n", and the comment */
    memcpy(t, b.p, b.len), t[b.len] = 0;
    size_t n = 0, line = 0, longest = 0;
    for (const char *p = t; *p; p++) {
        if (*p == '\n') u[n++] = '\r', line = 0;
        else if (++line > longest) longest = line;
        u[n++] = *p;
    }
    static const char END[] = "\n# the end\n";
    memcpy(u + n, END, sizeof END);
    netfile a, c;
    g_net_runs++;
    if (netfile_parse(t, &a, err, sizeof err)) {
        g_net_parsed++;
        if (longest + 1 < NETFILE_LINE_MAX && (!netfile_parse(u, &c, err, sizeof err) || memcmp(a.fp, c.fp, 32) || a.opts_len != c.opts_len ||
                                                memcmp(a.opts, c.opts, a.opts_len)))
            dump("network file: another fingerprint with \"\\r\\n\" and a comment", b.p, b.len);
    }
    free(t), free(u), free(b.p);
}

int main(int argc, char **argv)
{
    const double seconds = argc > 1 ? atof(argv[1]) : 10;
    g_x = argc > 2 ? strtoull(argv[2], NULL, 0) : (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ull;
    if (!g_x) g_x = 1;
    printf("fuzz_peer: %.0f s, seed %llu\n", seconds, (unsigned long long)g_x);
    buf js[NV(JSON_SEEDS)], hs[NV(HTTP_SEEDS)], ts[NV(MAIN_TX) + NV(TX_VEC) + NV(SIGN_VEC) + 1];
    for (size_t i = 0; i < NV(JSON_SEEDS); i++) load(&js[i], JSON_SEEDS[i], strlen(JSON_SEEDS[i]), 0);
    for (size_t i = 0; i < NV(HTTP_SEEDS); i++) load(&hs[i], HTTP_SEEDS[i], strlen(HTTP_SEEDS[i]), 0);
    size_t nts = 0;
    const char *txs[NV(ts)];
    for (size_t i = 0; i < NV(MAIN_TX); i++) txs[nts++] = MAIN_TX[i].raw;
    for (size_t i = 0; i < NV(TX_VEC); i++) txs[nts++] = TX_VEC[i].tx;
    for (size_t i = 0; i < NV(SIGN_VEC); i++) txs[nts++] = SIGN_VEC[i].signed_tx;
    txs[nts++] = BIP143_TX;
    for (size_t i = 0; i < nts; i++) {
        size_t n = 0;
        uint8_t *raw = pt_unhex_alloc(txs[i], &n);
        CHECK(raw != NULL);
        load(&ts[i], raw ? raw : (uint8_t *)"", n, 0);
        free(raw);
    }
    /* the signed spend: the first input of the first signing vector, with the script it spends */
    btc_tx signed_tx;
    uint8_t spk[BTC_P2PKH_LEN];
    CHECK(btc_tx_parse(&signed_tx, ts[NV(MAIN_TX) + NV(TX_VEC)].p, ts[NV(MAIN_TX) + NV(TX_VEC)].len));
    CHECK(pt_unhex(SIGN_VEC[0].spk[0], spk, sizeof spk) == BTC_P2PKH_LEN && btc_verify_p2pkh(&signed_tx, 0, spk, sizeof spk));
    /* records of the three types, and claims of 0, 3 and 64 proof keys; a network of records and one of blocks for their checks */
    uint8_t pub[33], base[32] = {[31] = 2};                     /* the key G, the start 2: Q' = -G */
    CHECK(pt_unhex("0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", pub, 33) == 33);
    CHECK(record_net_init(keys_ctx(), &g_net, (const uint8_t *)"fingerprint of the fuzzed network", 8, pub, base));
    g_blocks.bits = 36, g_blocks.start[27] = 0x10, memset(g_blocks.end + 26, 0xff, 6);
    buf rs[6], cs[3], nfs[NV(NET_SEEDS)];
    for (int i = 0; i < 3; i++) {
        record r = {.t = i - 1, .dp = 8 + (unsigned)i};
        uint8_t e[RECORD_LEN];
        CHECK(sys_random(r.x, 32) && sys_random(r.owner, 32));
        record_encode(&r, e);
        load(&rs[2 * i], e, RECORD_LEN, 0), load(&rs[2 * i + 1], e, RECORD_WIRE, 0);
    }
    for (int i = 0; i < 3; i++) {
        block_claim c = {.index = 7u << i, .n = i ? (i == 1 ? 3 : BLOCK_PROOFS_MAX) : 0};
        uint8_t e[BLOCK_CLAIM_MAX];
        for (int k = 0; k < c.n; k++) c.off[k] = 1000u * (uint64_t)k + 5;
        CHECK(sys_random(c.owner, 32) && sys_random(c.sig, 64));
        load(&cs[i], e, block_claim_encode(&c, e), 0);
    }
    for (size_t i = 0; i < NV(NET_SEEDS); i++) load(&nfs[i], NET_SEEDS[i], strlen(NET_SEEDS[i]), 0);
    if (g_fails) return 1;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long rounds = 0;
    for (;; rounds++) {
        if (!(rounds & 255)) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 >= seconds) break;
        }
        fuzz_json(js, NV(js));
        fuzz_tx(ts, nts);
        fuzz_http(hs, NV(hs));
        fuzz_record(rs, NV(rs));
        fuzz_claim(cs, NV(cs));
        fuzz_netfile(nfs, NV(nfs));
        if (!(rounds & 7)) fuzz_sig(&signed_tx, spk);
    }
    printf("fuzz_peer: OK, %lu rounds: JSON %lu (%lu parsed), transactions %lu (%lu parsed, %lu hashed), HTTP %lu (%lu complete), records %lu "
           "(%lu taken), claims %lu (%lu taken), network files %lu (%lu parsed)\n", rounds, g_json_runs, g_json_parsed, g_tx_runs, g_tx_parsed,
           g_tx_hashed, g_http_runs, g_http_done, g_rec_runs, g_rec_taken, g_claim_runs, g_claim_taken, g_net_runs, g_net_parsed);
    for (size_t i = 0; i < NV(js); i++) free(js[i].p);
    for (size_t i = 0; i < NV(hs); i++) free(hs[i].p);
    for (size_t i = 0; i < nts; i++) free(ts[i].p);
    for (size_t i = 0; i < NV(rs); i++) free(rs[i].p);
    for (size_t i = 0; i < NV(cs); i++) free(cs[i].p);
    for (size_t i = 0; i < NV(nfs); i++) free(nfs[i].p);
    return 0;
}
