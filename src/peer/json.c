/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* json.c - the JSON reader and writer of pzpeer (json.h). */
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_digit(unsigned c) { return c - '0' < 10; }

static int hex_digit(unsigned c)
{
    if (c - '0' < 10) return (int)(c - '0');
    c |= 0x20;
    if (c - 'a' < 6) return (int)(c - 'a' + 10);
    return -1;
}

/* the length of the valid UTF-8 sequence at p (RFC 3629: no overlong form, no surrogate, nothing above U+10FFFF), or 0 */
static size_t utf8_len(const unsigned char *p, const unsigned char *end)
{
    const unsigned c = p[0];
    if (c < 0x80) return 1;
    size_t n;
    unsigned lo = 0x80, hi = 0xbf;   /* the range of the second byte, narrower after the lead bytes that would allow those forms */
    if (c >= 0xc2 && c <= 0xdf) n = 2;
    else if (c >= 0xe0 && c <= 0xef) { n = 3; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 4; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
    else return 0;
    if ((size_t)(end - p) < n || p[1] < lo || p[1] > hi) return 0;
    for (size_t i = 2; i < n; i++)
        if ((p[i] & 0xc0) != 0x80) return 0;
    return n;
}

/* ---- reader ---- */

/* The text of the document (doc->text) holds the decoded strings and the numbers, each with a NUL. It never needs more than the
 * length of the input plus one: a string decodes to at most the bytes between its quotes, and its NUL takes the place of a quote; a
 * number is copied as it is and its NUL takes the place of the byte after it (a space, a comma or a bracket, which write nothing),
 * except for the number that ends the input. */
typedef struct {
    const unsigned char *p, *end;
    json_val *v;
    size_t count, cap;
    char *out;
    unsigned depth;
} jp;

static json_val *jp_node(jp *q, json_type t)
{
    if (q->count == q->cap) {
        if (q->cap >= JSON_MAX_NODES) return NULL;
        size_t cap = q->cap ? q->cap * 2 : 64;
        if (cap > JSON_MAX_NODES) cap = JSON_MAX_NODES;
        json_val *v = realloc(q->v, cap * sizeof *v);
        if (!v) return NULL;
        q->v = v;
        q->cap = cap;
    }
    json_val *v = &q->v[q->count++];
    *v = (json_val){.type = t, .span = 1};
    return v;
}

static void jp_ws(jp *q)
{
    while (q->p < q->end && (*q->p == ' ' || *q->p == '\t' || *q->p == '\n' || *q->p == '\r')) q->p++;
}

static long hex4(const unsigned char *p)
{
    long v = 0;
    for (int i = 0; i < 4; i++) {
        const int d = hex_digit(p[i]);
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

static char *put_utf8(char *o, unsigned long cp)
{
    if (cp < 0x80) { *o++ = (char)cp; return o; }
    if (cp < 0x800) { *o++ = (char)(0xc0 | cp >> 6); }
    else {
        if (cp < 0x10000) *o++ = (char)(0xe0 | cp >> 12);
        else { *o++ = (char)(0xf0 | cp >> 18); *o++ = (char)(0x80 | (cp >> 12 & 0x3f)); }
        *o++ = (char)(0x80 | (cp >> 6 & 0x3f));
    }
    *o++ = (char)(0x80 | (cp & 0x3f));
    return o;
}

static bool jp_string(jp *q)   /* q->p is at the opening quote */
{
    json_val *v = jp_node(q, JSON_STRING);
    if (!v) return false;
    char *const start = q->out;
    char *o = start;
    const unsigned char *p = q->p + 1, *const end = q->end;
    for (;;) {
        if (p >= end) return false;
        const unsigned c = *p;
        if (c == '"') break;
        if (c < 0x20) return false;
        if (c != '\\') {
            const size_t n = utf8_len(p, end);
            if (!n) return false;
            memcpy(o, p, n);
            o += n;
            p += n;
            continue;
        }
        if (end - p < 2) return false;
        const unsigned e = p[1];
        p += 2;
        switch (e) {
        case '"': case '\\': case '/': *o++ = (char)e; continue;
        case 'b': *o++ = '\b'; continue;
        case 'f': *o++ = '\f'; continue;
        case 'n': *o++ = '\n'; continue;
        case 'r': *o++ = '\r'; continue;
        case 't': *o++ = '\t'; continue;
        case 'u': break;
        default: return false;
        }
        if (end - p < 4) return false;
        long cp = hex4(p);
        p += 4;
        if (cp < 0 || (cp >= 0xdc00 && cp <= 0xdfff)) return false;   /* not hex, or a low surrogate alone */
        if (cp >= 0xd800 && cp <= 0xdbff) {                           /* a high surrogate: its low one must follow */
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
            const long lo = hex4(p + 2);
            if (lo < 0xdc00 || lo > 0xdfff) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            p += 6;
        }
        o = put_utf8(o, (unsigned long)cp);
    }
    *o = 0;
    v->s = start;
    v->n = (uint32_t)(o - start);
    q->out = o + 1;
    q->p = p + 1;
    return true;
}

static bool jp_number(jp *q)
{
    const unsigned char *p = q->p, *const end = q->end, *const s = p;
    if (p < end && *p == '-') p++;
    if (p >= end) return false;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (p < end && is_digit(*p)) p++;
    else return false;
    if (p < end && *p == '.') {
        if (++p >= end || !is_digit(*p)) return false;
        while (p < end && is_digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        if (++p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || !is_digit(*p)) return false;
        while (p < end && is_digit(*p)) p++;
    }
    json_val *v = jp_node(q, JSON_NUMBER);
    if (!v) return false;
    const size_t n = (size_t)(p - s);
    memcpy(q->out, s, n);
    q->out[n] = 0;
    v->s = q->out;
    v->n = (uint32_t)n;
    q->out += n + 1;
    q->p = p;
    return true;
}

static bool jp_word(jp *q, const char *w, json_type t)
{
    const size_t n = strlen(w);
    if ((size_t)(q->end - q->p) < n || memcmp(q->p, w, n) != 0) return false;
    q->p += n;
    return jp_node(q, t) != NULL;
}

static bool jp_value(jp *q);

static bool jp_container(jp *q)   /* q->p is at the opening bracket */
{
    const bool obj = *q->p == '{';
    const unsigned char close = obj ? '}' : ']';
    if (q->depth >= JSON_MAX_DEPTH || !jp_node(q, obj ? JSON_OBJECT : JSON_ARRAY)) return false;
    q->depth++;
    const size_t at = q->count - 1;   /* an index: the nodes move when they grow */
    uint32_t n = 0;
    q->p++;
    jp_ws(q);
    if (q->p < q->end && *q->p == close) q->p++;
    else for (;;) {
        if (obj) {
            jp_ws(q);
            if (q->p >= q->end || *q->p != '"' || !jp_string(q)) return false;
            jp_ws(q);
            if (q->p >= q->end || *q->p != ':') return false;
            q->p++;
        }
        if (!jp_value(q)) return false;
        n++;
        jp_ws(q);
        if (q->p >= q->end) return false;
        if (*q->p == ',') { q->p++; continue; }
        if (*q->p != close) return false;
        q->p++;
        break;
    }
    q->v[at].n = n;
    q->v[at].span = (uint32_t)(q->count - at);
    q->depth--;
    return true;
}

static bool jp_value(jp *q)
{
    jp_ws(q);
    if (q->p >= q->end) return false;
    switch (*q->p) {
    case '"': return jp_string(q);
    case '{': case '[': return jp_container(q);
    case 't': return jp_word(q, "true", JSON_TRUE);
    case 'f': return jp_word(q, "false", JSON_FALSE);
    case 'n': return jp_word(q, "null", JSON_NULL);
    default: return jp_number(q);
    }
}

bool json_parse(json_doc *doc, const char *text, size_t len)
{
    *doc = (json_doc){0};
    if (!text || len > JSON_MAX_SIZE) return false;
    char *buf = malloc(len + 1);
    if (!buf) return false;
    jp q = {.p = (const unsigned char *)text, .end = (const unsigned char *)text + len, .out = buf};
    bool ok = jp_value(&q);
    if (ok) {
        jp_ws(&q);
        ok = q.p == q.end;
    }
    if (!ok) {
        free(q.v);
        free(buf);
        return false;
    }
    doc->v = q.v;
    doc->count = q.count;
    doc->text = buf;
    return true;
}

void json_free(json_doc *doc)
{
    free(doc->v);
    free(doc->text);
    *doc = (json_doc){0};
}

const json_val *json_get(const json_val *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    const size_t kl = strlen(key);
    const json_val *k = obj + 1;
    for (uint32_t i = 0; i < obj->n; i++) {
        const json_val *v = k + 1;
        if (k->n == kl && memcmp(k->s, key, kl) == 0) return v;
        k = json_next(v);
    }
    return NULL;
}

const json_val *json_at(const json_val *arr, size_t i)
{
    if (!arr || arr->type != JSON_ARRAY || i >= arr->n) return NULL;
    const json_val *v = arr + 1;
    while (i--) v = json_next(v);
    return v;
}

const char *json_str(const json_val *v)
{
    if (!v || v->type != JSON_STRING || memchr(v->s, 0, v->n)) return NULL;
    return v->s;
}

/* the digits s[0..n) as an integer; false when one is not a digit, there are none, or the value is above max */
static bool digits_u64(const char *s, size_t n, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    if (!n) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned d = (unsigned char)s[i] - '0';
        if (d > 9 || v > (max - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool json_u64(const json_val *v, uint64_t *out)
{
    return v && v->type == JSON_NUMBER && digits_u64(v->s, v->n, UINT64_MAX, out);
}

bool json_i64(const json_val *v, int64_t *out)
{
    if (!v || v->type != JSON_NUMBER) return false;
    const bool neg = v->s[0] == '-';
    uint64_t m;
    if (!digits_u64(v->s + neg, v->n - neg, (uint64_t)INT64_MAX + neg, &m)) return false;
    *out = neg ? (int64_t)(0 - m) : (int64_t)m;   /* 0 - 2^63 wraps to INT64_MIN, as wanted */
    return true;
}

bool json_bool(const json_val *v, bool *out)
{
    if (!v || (v->type != JSON_TRUE && v->type != JSON_FALSE)) return false;
    *out = v->type == JSON_TRUE;
    return true;
}

bool json_parse_sats(const char *s, size_t len, uint64_t *sats)
{
    /* The digits of the number without its point make an integer D, and the amount in satoshis is D * 10^scale with
     * scale = 8 - (digits after the point) + exponent. It is exact when the digits that a negative scale drops are all zeros. */
    if (!s || !len) return false;
    const char *p = s, *const end = s + len, *const i0 = p;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (p < end && is_digit((unsigned char)*p)) p++;
    else return false;   /* also the sign of a negative amount */
    const char *const i1 = p;
    const char *f0 = p, *f1 = p;
    if (p < end && *p == '.') {
        f0 = ++p;
        while (p < end && is_digit((unsigned char)*p)) p++;
        f1 = p;
        if (f0 == f1) return false;
    }
    long exp = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        bool neg = false;
        if (++p < end && (*p == '+' || *p == '-')) neg = *p++ == '-';
        if (p >= end || !is_digit((unsigned char)*p)) return false;
        for (; p < end && is_digit((unsigned char)*p); p++)
            if (exp < 100000) exp = exp * 10 + (*p - '0');   /* beyond that any non-zero amount is out of range either way */
        if (neg) exp = -exp;
    }
    if (p != end) return false;
    const size_t ni = (size_t)(i1 - i0), nd = ni + (size_t)(f1 - f0);
    const long scale = 8 - (long)(f1 - f0) + exp;
    const size_t keep = scale >= 0 ? nd : (size_t)-scale >= nd ? 0 : nd - (size_t)-scale;
    uint64_t v = 0;
    for (size_t k = 0; k < nd; k++) {
        const unsigned d = (unsigned)((k < ni ? i0[k] : f0[k - ni]) - '0');
        if (k >= keep) {
            if (d) return false;   /* a fraction of a satoshi */
            continue;
        }
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    for (long k = 0; v && k < scale; k++) {
        if (v > UINT64_MAX / 10) return false;
        v *= 10;
    }
    if (v > JSON_MAX_SATS) return false;
    *sats = v;
    return true;
}

bool json_sats(const json_val *v, uint64_t *sats)
{
    return v && v->type == JSON_NUMBER && json_parse_sats(v->s, v->n, sats);
}

/* ---- writer ---- */

void jw_init(json_w *w) { *w = (json_w){0}; }

void jw_free(json_w *w)
{
    free(w->p);
    *w = (json_w){0};
}

static void jw_put(json_w *w, const char *s, size_t n)
{
    if (w->err) return;
    if (w->cap - w->len <= n) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap - w->len <= n) {
            if (cap > JSON_MAX_SIZE) { w->err = true; return; }
            cap *= 2;
        }
        char *p = realloc(w->p, cap);
        if (!p) { w->err = true; return; }
        w->p = p;
        w->cap = cap;
    }
    memcpy(w->p + w->len, s, n);
    w->len += n;
    w->p[w->len] = 0;
}

/* before a value: the comma that separates it from the item before, and inside an object the check that its key came first */
static void jw_item(json_w *w)
{
    if (!w->depth) {
        if (w->len) w->err = true;   /* one value per text */
        return;
    }
    const uint32_t bit = 1u << (w->depth - 1);
    if (w->obj & bit) {
        if (!w->key) w->err = true;
        w->key = false;               /* the comma went before the key */
        return;
    }
    if (w->more & bit) jw_put(w, ",", 1);
    w->more |= bit;
}

static void jw_quoted(json_w *w, const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s, *const end = p + n;
    jw_put(w, "\"", 1);
    while (p < end && !w->err) {
        const unsigned char *run = p;   /* what goes out as it is */
        while (p < end && *p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\') p++;
        jw_put(w, (const char *)run, (size_t)(p - run));
        if (p == end) break;
        if (*p >= 0x80) {
            const size_t k = utf8_len(p, end);
            if (!k) { w->err = true; break; }
            jw_put(w, (const char *)p, k);
            p += k;
            continue;
        }
        char e[8];
        switch (*p) {
        case '"': strcpy(e, "\\\""); break;
        case '\\': strcpy(e, "\\\\"); break;
        case '\n': strcpy(e, "\\n"); break;
        case '\r': strcpy(e, "\\r"); break;
        case '\t': strcpy(e, "\\t"); break;
        default: snprintf(e, sizeof e, "\\u%04x", *p); break;
        }
        jw_put(w, e, strlen(e));
        p++;
    }
    jw_put(w, "\"", 1);
}

static void jw_open(json_w *w, bool obj)
{
    jw_item(w);
    if (w->depth >= JSON_MAX_DEPTH) { w->err = true; return; }
    const uint32_t bit = 1u << w->depth++;
    if (obj) w->obj |= bit;
    else w->obj &= ~bit;
    w->more &= ~bit;
    jw_put(w, obj ? "{" : "[", 1);
}

void jw_obj(json_w *w) { jw_open(w, true); }
void jw_arr(json_w *w) { jw_open(w, false); }

void jw_end(json_w *w)
{
    if (!w->depth || w->key) { w->err = true; return; }
    const uint32_t bit = 1u << --w->depth;
    jw_put(w, w->obj & bit ? "}" : "]", 1);
}

void jw_key(json_w *w, const char *k)
{
    const uint32_t bit = w->depth ? 1u << (w->depth - 1) : 0;
    if (!(w->obj & bit) || w->key || !k) { w->err = true; return; }
    if (w->more & bit) jw_put(w, ",", 1);
    w->more |= bit;
    jw_quoted(w, k, strlen(k));
    jw_put(w, ":", 1);
    w->key = true;
}

void jw_strn(json_w *w, const char *s, size_t n)
{
    if (!s && n) { w->err = true; return; }
    jw_item(w);
    jw_quoted(w, s ? s : "", n);
}

void jw_str(json_w *w, const char *s)
{
    if (!s) { w->err = true; return; }
    jw_strn(w, s, strlen(s));
}

static void jw_text(json_w *w, const char *t)
{
    jw_item(w);
    jw_put(w, t, strlen(t));
}

void jw_u64(json_w *w, uint64_t v)
{
    char t[24];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    jw_text(w, t);
}

void jw_i64(json_w *w, int64_t v)
{
    char t[24];
    snprintf(t, sizeof t, "%lld", (long long)v);
    jw_text(w, t);
}

void jw_bool(json_w *w, bool v) { jw_text(w, v ? "true" : "false"); }
void jw_null(json_w *w) { jw_text(w, "null"); }

void jw_sats(json_w *w, uint64_t sats)
{
    char t[32];
    snprintf(t, sizeof t, "%llu.%08llu", (unsigned long long)(sats / 100000000), (unsigned long long)(sats % 100000000));
    jw_text(w, t);
}

void jw_hex(json_w *w, const uint8_t *p, size_t n)
{
    static const char X[] = "0123456789abcdef";
    if (!p && n) { w->err = true; return; }
    jw_item(w);
    jw_put(w, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        const char h[2] = {X[p[i] >> 4], X[p[i] & 15]};
        jw_put(w, h, 2);
    }
    jw_put(w, "\"", 1);
}

void jw_raw(json_w *w, const char *json, size_t n)
{
    if (!json || !n) { w->err = true; return; }
    jw_item(w);
    jw_put(w, json, n);
}

const char *jw_done(json_w *w, size_t *len)
{
    if (w->err || w->depth || w->key || !w->len) return NULL;
    if (len) *len = w->len;
    return w->p;
}
