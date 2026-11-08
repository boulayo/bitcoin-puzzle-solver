/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_json.c - the JSON reader and writer of pzpeer: the grammar and its limits, escapes and UTF-8, exact amounts, the writer. */
#include "json.h"
#include "peertest.h"

static bool parse_str(json_doc *d, const char *s) { return json_parse(d, s, strlen(s)); }

static bool accepts(const char *s)
{
    json_doc d;
    const bool ok = parse_str(&d, s);
    json_free(&d);
    return ok;
}

static void test_valid(void)
{
    json_doc d;
    const char *text = " {\"result\": {\"txid\": \"ab\", \"vout\": 3, \"list\": [1, -2.5e+3, [], {}, [true, false, null]], "
                       "\"amount\": 0.00012345, \"empty\": \"\"}, \"error\": null, \"id\": 18446744073709551615}\r\n\t";
    CHECK(parse_str(&d, text));
    const json_val *root = d.v;
    CHECK(root->type == JSON_OBJECT && root->n == 3 && root->span == d.count);
    const json_val *res = json_get(root, "result");
    CHECK(res && res->type == JSON_OBJECT && res->n == 5);
    CHECK_STR(json_str(json_get(res, "txid")), "ab");
    CHECK_STR(json_str(json_get(res, "empty")), "");
    uint64_t u;
    CHECK(json_u64(json_get(res, "vout"), &u) && u == 3);
    CHECK(json_u64(json_get(root, "id"), &u) && u == UINT64_MAX);
    CHECK(json_is_null(json_get(root, "error")));
    CHECK(!json_get(root, "missing") && !json_get(root, "resul") && !json_get(res, "result"));
    const json_val *list = json_get(res, "list");
    CHECK(list && list->type == JSON_ARRAY && list->n == 5);
    CHECK(json_at(list, 1)->type == JSON_NUMBER);
    CHECK_STR(json_at(list, 1)->s, "-2.5e+3");   /* the text as written */
    CHECK(json_at(list, 2)->type == JSON_ARRAY && json_at(list, 2)->n == 0);
    CHECK(json_at(list, 3)->type == JSON_OBJECT && json_at(list, 3)->n == 0);
    const json_val *in = json_at(list, 4);
    bool b;
    CHECK(in && in->n == 3 && json_bool(json_at(in, 0), &b) && b && json_bool(json_at(in, 1), &b) && !b);
    CHECK(json_is_null(json_at(in, 2)) && !json_at(in, 3) && !json_at(res, 0));
    /* json_next walks the elements: the one after the nested array is the amount key */
    CHECK(json_next(list)->type == JSON_STRING && !strcmp(json_next(list)->s, "amount"));
    CHECK(json_sats(json_get(res, "amount"), &u) && u == 12345);
    /* the wrong types read as absent */
    CHECK(!json_str(json_get(res, "vout")) && !json_u64(json_get(res, "txid"), &u) && !json_bool(list, &b));
    CHECK(!json_get(NULL, "x") && !json_at(NULL, 0) && !json_str(NULL) && !json_is_null(NULL));
    json_free(&d);
    json_free(&d);   /* twice is harmless */

    const char *scalars[] = {"0", "-0", "1.5", "1E9", "1e-9", "true", "false", "null", "\"\"", "[]", "{}", " [ ] ", "[[[]]]",
                             "{\"\":0}", "{\"a\":{\"a\":{\"a\":[]}}}", "-1234567890.0987654321e-0012"};
    for (size_t i = 0; i < sizeof scalars / sizeof scalars[0]; i++) CHECK(accepts(scalars[i]));
}

static void test_strings(void)
{
    json_doc d;
    /* every escape; a BMP character as \u and as UTF-8; a pair of surrogates; the bytes as they must come out */
    CHECK(parse_str(&d, "\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\\u0041\\u00e9\\u20ac\\ud83d\\ude00\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\""));
    CHECK_STR(json_str(d.v), "a\"b\\c/d\be\ff\ng\rh\tiA\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    json_free(&d);
    /* the largest code point, in both forms */
    CHECK(parse_str(&d, "[\"\\udbff\\udfff\", \"\xf4\x8f\xbf\xbf\", \"\\uD800\\uDC00\"]"));
    CHECK_STR(json_str(json_at(d.v, 0)), "\xf4\x8f\xbf\xbf");
    CHECK_STR(json_str(json_at(d.v, 1)), "\xf4\x8f\xbf\xbf");
    CHECK_STR(json_str(json_at(d.v, 2)), "\xf0\x90\x80\x80");
    json_free(&d);
    /* a NUL inside: kept, with its length, but not handed out as a C string */
    CHECK(parse_str(&d, "\"ab\\u0000cd\""));
    CHECK(d.v->type == JSON_STRING && d.v->n == 5 && !memcmp(d.v->s, "ab\0cd", 5) && !json_str(d.v));
    json_free(&d);
    /* a key with an escape matches its decoded form */
    CHECK(parse_str(&d, "{\"k\\u0065y\": 1}"));
    CHECK(json_get(d.v, "key") != NULL);
    json_free(&d);
}

static void test_invalid(void)
{
    static const char *const bad[] = {
        "", " ", "[", "]", "{", "}", "[1,]", "[,1]", "[1 2]", "{\"a\":1,}", "{\"a\"}", "{\"a\" 1}", "{a:1}", "{1:1}", "{\"a\":}",
        "01", "-01", "00", "-", "1.", ".5", "1e", "1e+", "+1", "1.e5", "0x10", "NaN", "Infinity", "-Infinity", "1 2", "[] []",
        "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\u12g4\"", "\"\\U0041\"", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"",
        "\"\\ud800\\ud800\"", "\"\\ud800x\"", "\"\t\"", "\"\n\"", "\"\x1f\"", "\"\xc0\x80\"", "\"\xc1\xbf\"", "\"\xe0\x80\x80\"",
        "\"\xed\xa0\x80\"", "\"\xf0\x80\x80\x80\"", "\"\xf4\x90\x80\x80\"", "\"\xf5\x80\x80\x80\"", "\"\x80\"", "\"\xbf\"",
        "\"\xe2\x82\"", "\"\xe2\x82\xac", "\"\xff\"", "tru", "nul", "fals", "truee", "True", "null null", "[] x", "{\"a\":1}}",
        "[1]]", "'a'", "[01]", "[-]", "{\"a\":1 \"b\":2}", "\xef\xbb\xbf[]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (accepts(bad[i])) {
            printf("FAIL accepted: \"%s\"\n", bad[i]);
            g_fails++;
        }
    /* a NUL byte in the text, outside and inside a string */
    json_doc d;
    CHECK(!json_parse(&d, "[1]\0", 4) && !json_parse(&d, "\"a\0b\"", 5) && !json_parse(&d, NULL, 0));
    CHECK(d.v == NULL && d.text == NULL && d.count == 0);   /* empty after a failure */
    /* the text is taken to len exactly: a prefix may be valid where the whole is not */
    CHECK(json_parse(&d, "[1]x", 3));
    json_free(&d);
}

static void test_limits(void)
{
    /* depth: JSON_MAX_DEPTH nested containers are taken, one more is not */
    char deep[2 * JSON_MAX_DEPTH + 8];
    for (int extra = 0; extra < 2; extra++) {
        const int n = JSON_MAX_DEPTH + extra;
        for (int i = 0; i < n; i++) { deep[i] = '['; deep[2 * n - 1 - i] = ']'; }
        deep[2 * n] = 0;
        CHECK(accepts(deep) == !extra);
    }
    char deepo[8 * JSON_MAX_DEPTH + 8], *p = deepo;
    for (int i = 0; i <= JSON_MAX_DEPTH; i++) p += sprintf(p, "{\"a\":");
    p += sprintf(p, "1");
    for (int i = 0; i <= JSON_MAX_DEPTH; i++) *p++ = '}';
    *p = 0;
    CHECK(!accepts(deepo));

    /* nodes: an array of JSON_MAX_NODES - 1 zeros is JSON_MAX_NODES nodes; one more is too many */
    const size_t cnt = JSON_MAX_NODES - 1, len = 2 * (cnt + 1) + 1;
    char *t = malloc(len + 1);
    CHECK(t != NULL);
    if (t) {
        size_t o = 0;
        t[o++] = '[';
        for (size_t i = 0; i < cnt; i++) { t[o++] = '0'; t[o++] = ','; }
        t[o - 1] = ']';
        t[o] = 0;
        json_doc d;
        CHECK(json_parse(&d, t, o) && d.count == JSON_MAX_NODES && d.v->n == cnt);
        json_free(&d);
        t[o - 1] = ',';
        t[o++] = '0';
        t[o++] = ']';
        CHECK(!json_parse(&d, t, o));
        free(t);
    }
    /* size: one byte over JSON_MAX_SIZE is refused before it is read */
    char *big = malloc(JSON_MAX_SIZE + 1);
    CHECK(big != NULL);
    if (big) {
        memset(big, ' ', JSON_MAX_SIZE + 1);
        big[0] = '1';
        json_doc d;
        CHECK(!json_parse(&d, big, JSON_MAX_SIZE + 1));
        CHECK(json_parse(&d, big, JSON_MAX_SIZE));   /* a number and spaces */
        json_free(&d);
        /* one string of the whole size */
        big[0] = '"';
        memset(big + 1, 'x', JSON_MAX_SIZE - 2);
        big[JSON_MAX_SIZE - 1] = '"';
        CHECK(json_parse(&d, big, JSON_MAX_SIZE) && d.v->n == JSON_MAX_SIZE - 2);
        json_free(&d);
        free(big);
    }
}

static void test_ints(void)
{
    static const struct { const char *s; int u_ok; uint64_t u; int i_ok; int64_t i; } V[] = {
        {"0", 1, 0, 1, 0},
        {"-0", 0, 0, 1, 0},
        {"7", 1, 7, 1, 7},
        {"18446744073709551615", 1, UINT64_MAX, 0, 0},
        {"18446744073709551616", 0, 0, 0, 0},
        {"99999999999999999999", 0, 0, 0, 0},
        {"9223372036854775807", 1, 9223372036854775807ull, 1, INT64_MAX},
        {"9223372036854775808", 1, 9223372036854775808ull, 0, 0},
        {"-9223372036854775808", 0, 0, 1, INT64_MIN},
        {"-9223372036854775809", 0, 0, 0, 0},
        {"-1", 0, 0, 1, -1},
        {"1.0", 0, 0, 0, 0},
        {"1e3", 0, 0, 0, 0},
    };
    for (size_t k = 0; k < sizeof V / sizeof V[0]; k++) {
        json_doc d;
        uint64_t u = 12345;
        int64_t i = 12345;
        CHECK(parse_str(&d, V[k].s));
        const bool uo = json_u64(d.v, &u), io = json_i64(d.v, &i);
        if (uo != V[k].u_ok || (uo && u != V[k].u) || io != V[k].i_ok || (io && i != V[k].i)) {
            printf("FAIL integer %s\n", V[k].s);
            g_fails++;
        }
        json_free(&d);
    }
}

static void test_sats(void)
{
    static const struct { const char *s; int ok; uint64_t sats; } V[] = {
        {"0", 1, 0},
        {"0.0", 1, 0},
        {"0.00000001", 1, 1},
        {"0.00012345", 1, 12345},
        {"0.1", 1, 10000000},
        {"1", 1, 100000000},
        {"6.25", 1, 625000000},
        {"50.00000000", 1, 5000000000ull},
        {"20999999.99999999", 1, 2099999999999999ull},
        {"21000000", 1, 2100000000000000ull},
        {"21000000.00000000000", 1, 2100000000000000ull},
        {"21000000.00000001", 0, 0},
        {"21000001", 0, 0},
        {"0.000000001", 0, 0},
        {"0.000000010", 1, 1},
        {"0.0000000100000000000000000000", 1, 1},
        {"0.00000001000000000000000000001", 0, 0},
        {"1e-8", 1, 1},
        {"1E-08", 1, 1},
        {"1e-9", 0, 0},
        {"10e-9", 1, 1},
        {"1.5e2", 1, 15000000000ull},
        {"12345e-8", 1, 12345},
        {"0.1e-7", 1, 1},
        {"0.01e-7", 0, 0},
        {"2.1e7", 1, 2100000000000000ull},
        {"2.1E+7", 1, 2100000000000000ull},
        {"2.1e8", 0, 0},
        {"1e100", 0, 0},
        {"1e99999999999999999999", 0, 0},
        {"0e99999999999999999999", 1, 0},
        {"0e-99999999999999999999", 1, 0},
        {"1e-99999999999999999999", 0, 0},
        {"0.00000000000000000000000000000000000000000000000000", 1, 0},
        {"184467440737.09551616", 0, 0},
        {"99999999999999999999999999", 0, 0},
        {"-0", 0, 0},
        {"-1", 0, 0},
        {"-0.00000001", 0, 0},
        {"", 0, 0},
        {"1.", 0, 0},
        {".1", 0, 0},
        {"01", 0, 0},
        {"00.1", 0, 0},
        {"1 ", 0, 0},
        {" 1", 0, 0},
        {"0x10", 0, 0},
        {"1e", 0, 0},
        {"1e+", 0, 0},
        {"1,5", 0, 0},
    };
    for (size_t k = 0; k < sizeof V / sizeof V[0]; k++) {
        uint64_t s = 777;
        const bool ok = json_parse_sats(V[k].s, strlen(V[k].s), &s);
        if (ok != V[k].ok || (ok && s != V[k].sats) || (!ok && s != 777)) {
            printf("FAIL sats \"%s\": %d %llu\n", V[k].s, ok, (unsigned long long)s);
            g_fails++;
        }
    }
    /* only a number: the same digits as a string are not an amount */
    json_doc d;
    uint64_t s;
    CHECK(parse_str(&d, "[\"0.1\", 0.1]"));
    CHECK(!json_sats(json_at(d.v, 0), &s) && json_sats(json_at(d.v, 1), &s) && s == 10000000);
    json_free(&d);
    /* the writer's form reads back to the same amount, at the edges and at random */
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < 20000; k++) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        uint64_t v = (x * 0x2545F4914F6CDD1Dull) % (JSON_MAX_SATS + 1);
        if (k < 4) v = (uint64_t[]){0, 1, JSON_MAX_SATS, 99999999}[k];
        json_w w;
        jw_init(&w);
        jw_sats(&w, v);
        size_t n;
        const char *t = jw_done(&w, &n);
        uint64_t back = 0;
        if (!t || !json_parse(&d, t, n) || !json_sats(d.v, &back) || back != v) {
            printf("FAIL sats round trip %llu\n", (unsigned long long)v);
            g_fails++;
            k = 1 << 30;
        }
        json_free(&d);
        jw_free(&w);
    }
}

static void test_writer(void)
{
    json_w w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "jsonrpc");
    jw_str(&w, "1.0");
    jw_key(&w, "id");
    jw_u64(&w, 7);
    jw_key(&w, "method");
    jw_str(&w, "x");
    jw_key(&w, "params");
    jw_arr(&w);
    jw_str(&w, "a\"b\\c\n\r\t\x01\x1f\x7f\xc3\xa9/");
    jw_i64(&w, -5);
    jw_i64(&w, INT64_MIN);
    jw_u64(&w, UINT64_MAX);
    jw_bool(&w, true);
    jw_bool(&w, false);
    jw_null(&w);
    jw_sats(&w, 12345);
    jw_sats(&w, 2100000000000000ull);
    jw_hex(&w, (const uint8_t *)"\x00\xff\x10", 3);
    jw_arr(&w);
    jw_end(&w);
    jw_obj(&w);
    jw_end(&w);
    jw_strn(&w, "a\0b", 3);
    jw_raw(&w, "[1,{\"k\":2}]", 11);
    jw_end(&w);
    jw_end(&w);
    size_t n;
    const char *t = jw_done(&w, &n);
    CHECK_STR(t, "{\"jsonrpc\":\"1.0\",\"id\":7,\"method\":\"x\",\"params\":[\"a\\\"b\\\\c\\n\\r\\t\\u0001\\u001f\x7f\xc3\xa9/\",-5,"
                 "-9223372036854775808,18446744073709551615,true,false,null,0.00012345,21000000.00000000,\"00ff10\",[],{},"
                 "\"a\\u0000b\",[1,{\"k\":2}]]}");
    CHECK(t && n == strlen(t));
    json_doc d;
    CHECK(t && json_parse(&d, t, n));
    const json_val *p = json_get(d.v, "params");
    CHECK(p && p->n == 14);
    CHECK_STR(json_str(json_at(p, 0)), "a\"b\\c\n\r\t\x01\x1f\x7f\xc3\xa9/");
    json_free(&d);
    jw_free(&w);
    jw_free(&w);

    /* misuse: the text is withheld */
    jw_init(&w); jw_obj(&w); jw_u64(&w, 1); jw_end(&w); CHECK(!jw_done(&w, NULL)); jw_free(&w);            /* value without key */
    jw_init(&w); jw_arr(&w); jw_key(&w, "k"); jw_end(&w); CHECK(!jw_done(&w, NULL)); jw_free(&w);          /* key in an array */
    jw_init(&w); jw_obj(&w); jw_key(&w, "k"); jw_end(&w); CHECK(!jw_done(&w, NULL)); jw_free(&w);          /* key without value */
    jw_init(&w); jw_obj(&w); jw_key(&w, "k"); jw_key(&w, "j"); CHECK(!jw_done(&w, NULL)); jw_free(&w);     /* two keys */
    jw_init(&w); jw_arr(&w); CHECK(!jw_done(&w, NULL)); jw_free(&w);                                        /* open */
    jw_init(&w); jw_end(&w); CHECK(!jw_done(&w, NULL)); jw_free(&w);                                        /* closed, never opened */
    jw_init(&w); jw_u64(&w, 1); jw_u64(&w, 2); CHECK(!jw_done(&w, NULL)); jw_free(&w);                      /* two values */
    jw_init(&w); jw_key(&w, "k"); CHECK(!jw_done(&w, NULL)); jw_free(&w);                                   /* key outside */
    jw_init(&w); CHECK(!jw_done(&w, NULL)); jw_free(&w);                                                    /* nothing */
    jw_init(&w); jw_str(&w, "\xc0\x80"); CHECK(!jw_done(&w, NULL)); jw_free(&w);                            /* not UTF-8 */
    jw_init(&w); jw_str(&w, "\xe2\x82"); CHECK(!jw_done(&w, NULL)); jw_free(&w);
    jw_init(&w); jw_str(&w, NULL); CHECK(!jw_done(&w, NULL)); jw_free(&w);
    jw_init(&w);
    for (int i = 0; i <= JSON_MAX_DEPTH; i++) jw_arr(&w);
    for (int i = 0; i <= JSON_MAX_DEPTH; i++) jw_end(&w);
    CHECK(!jw_done(&w, NULL));
    jw_free(&w);
    jw_init(&w);
    for (int i = 0; i < JSON_MAX_DEPTH; i++) jw_arr(&w);
    for (int i = 0; i < JSON_MAX_DEPTH; i++) jw_end(&w);
    CHECK(jw_done(&w, NULL) && accepts(jw_done(&w, NULL)));
    jw_free(&w);

    /* strings of random code points and control characters come back as they went */
    uint64_t x = 88172645463325252ull;
    for (int k = 0; k < 3000; k++) {
        char s[400];
        size_t sl = 0;
        const int len = (int)(x % 60);
        for (int i = 0; i < len; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            unsigned long cp = (unsigned long)(x % 5 == 0 ? x % 0x20 : x % 5 == 1 ? x % 0x80 : x % 5 == 2 ? x % 0x800 : x % 0x110000);
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
            if (cp < 0x80) s[sl++] = (char)cp;
            else if (cp < 0x800) { s[sl++] = (char)(0xc0 | cp >> 6); s[sl++] = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { s[sl++] = (char)(0xe0 | cp >> 12); s[sl++] = (char)(0x80 | (cp >> 6 & 63)); s[sl++] = (char)(0x80 | (cp & 63)); }
            else {
                s[sl++] = (char)(0xf0 | cp >> 18); s[sl++] = (char)(0x80 | (cp >> 12 & 63));
                s[sl++] = (char)(0x80 | (cp >> 6 & 63)); s[sl++] = (char)(0x80 | (cp & 63));
            }
        }
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        jw_init(&w);
        jw_strn(&w, s, sl);
        const char *out = jw_done(&w, &n);
        if (!out || !json_parse(&d, out, n) || d.v->type != JSON_STRING || d.v->n != sl || memcmp(d.v->s, s, sl)) {
            printf("FAIL string round trip %d\n", k);
            g_fails++;
            k = 1 << 30;
        }
        json_free(&d);
        jw_free(&w);
    }
}

/* Mutations of a valid document: whatever the result, no read or write outside the buffers (the sanitizers watch) and a document
 * that parses has consistent spans. */
static void test_mutations(void)
{
    const char *seed = "{\"result\":{\"unspents\":[{\"txid\":\"00ff\",\"vout\":1,\"amount\":0.5e-3,\"s\":\"\\ud83d\\ude00\\n\"}],"
                       "\"x\":[true,false,null,-0.1E+2]},\"error\":null,\"id\":1}";
    const size_t n = strlen(seed);
    static const char alphabet[] = "{}[]\",:\\u0aeE.-+ tfn\x80\xc3\xbf\xed\xf4";
    uint64_t x = 0x0123456789abcdefull;
    int parsed = 0;
    for (int k = 0; k < 40000; k++) {
        char *b = malloc(n + 1);
        memcpy(b, seed, n + 1);
        size_t len = n;
        for (int m = 0; m < 1 + k % 3; m++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            const size_t at = x % len;
            switch ((x >> 20) % 3) {
            case 0: b[at] = alphabet[(x >> 32) % (sizeof alphabet - 1)]; break;
            case 1: len = at + 1; break;   /* cut */
            default: memmove(b + at, b + at + 1, len - at - 1); len--; break;   /* drop a byte */
            }
            if (!len) len = 1;
        }
        json_doc d;
        if (json_parse(&d, b, len)) {
            parsed++;
            CHECK(d.v[0].span == d.count);
            json_free(&d);
        }
        free(b);
    }
    CHECK(parsed > 0);
}

int main(void)
{
    test_valid();
    test_strings();
    test_invalid();
    test_limits();
    test_ints();
    test_sats();
    test_writer();
    test_mutations();
    TEST_DONE("t_json");
}
