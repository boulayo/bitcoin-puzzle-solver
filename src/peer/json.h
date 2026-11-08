/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* json.h - a small JSON reader (RFC 8259, into a tree of nodes) and writer, for the JSON-RPC of bitcoind and the jobs of pzfirma.
 *
 * The reader takes untrusted text. Anything outside the grammar is refused: bad escapes, lone surrogates, invalid UTF-8, control
 * characters inside strings, leading zeros, trailing commas, text after the value, and also a document deeper than JSON_MAX_DEPTH,
 * longer than JSON_MAX_SIZE bytes or with more than JSON_MAX_NODES values. Numbers keep the text they were written with, so that amounts
 * are read exactly (json_sats), never through a double.
 *
 * The writer puts the commas and the quotes itself and escapes strings, so a request built with it is valid JSON whatever the strings
 * hold; a misuse (a value where a key is due, unbalanced brackets, a string that is not UTF-8) makes jw_done return NULL. */
#ifndef PZPEER_JSON_H
#define PZPEER_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JSON_MAX_DEPTH 32
#define JSON_MAX_SIZE  (16u << 20)   /* a 4 MB transaction in hex, with room */
#define JSON_MAX_NODES (1u << 20)

typedef enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

/* One value. The nodes of a document are in document order: the contents of an array or an object follow it, an object as key and
 * value pairs (the key is a JSON_STRING node), and the value after v and everything inside it is v + v->span (json_next). */
typedef struct json_val {
    json_type type;
    uint32_t n;      /* string: bytes of the decoded text; number: bytes of its text; array: elements; object: members */
    uint32_t span;   /* the nodes of this value, itself included */
    const char *s;   /* string: the decoded UTF-8 text, NUL-terminated (it may hold a NUL of its own, see json_str); number: its text */
} json_val;

typedef struct {
    json_val *v;     /* v[0] is the root */
    size_t count;
    char *text;      /* where the strings and the numbers of the nodes live */
} json_doc;

/* Parses text[0..len) into doc. Returns false, with doc empty, on any error (malformed, over a limit, out of memory). The document is
 * freed with json_free, which also takes an empty one. */
bool json_parse(json_doc *doc, const char *text, size_t len);
void json_free(json_doc *doc);

/* Navigation. Each function takes NULL, or a value of another type, and then returns NULL. */
const json_val *json_get(const json_val *obj, const char *key);   /* the value of the first member with that key */
const json_val *json_at(const json_val *arr, size_t i);           /* element i */
static inline const json_val *json_next(const json_val *v) { return v + v->span; }

/* Typed reads: NULL or false when v is NULL or not of that type. */
const char *json_str(const json_val *v);                  /* a string without a NUL inside, as a C string */
bool json_u64(const json_val *v, uint64_t *out);          /* an integer from 0 to 2^64 - 1, written without fraction or exponent */
bool json_i64(const json_val *v, int64_t *out);           /* the same from -2^63 to 2^63 - 1 */
bool json_bool(const json_val *v, bool *out);
static inline bool json_is_null(const json_val *v) { return v && v->type == JSON_NULL; }

/* An amount in BTC (a JSON number, as bitcoind writes them: 0.00012345) to satoshis, exactly: refused when it is negative, has a
 * fraction of a satoshi or is above 21 million BTC. Exponents are taken (1e-05 is 1000 satoshis). json_parse_sats does the same on the
 * text of a number. */
#define JSON_MAX_SATS 2100000000000000ull
bool json_sats(const json_val *v, uint64_t *sats);
bool json_parse_sats(const char *s, size_t len, uint64_t *sats);

/* ---- writer ---- */

typedef struct {
    char *p;
    size_t len, cap;
    bool err;        /* out of memory or misuse: the text is not to be used */
    bool key;        /* a key was written and its value is due */
    unsigned depth;
    uint32_t obj;    /* bit d - 1: the container at depth d is an object */
    uint32_t more;   /* bit d - 1: it has something already, the next item takes a comma */
} json_w;

void jw_init(json_w *w);
void jw_free(json_w *w);
void jw_obj(json_w *w);                                  /* { */
void jw_arr(json_w *w);                                  /* [ */
void jw_end(json_w *w);                                  /* } or ], whichever is open */
void jw_key(json_w *w, const char *k);
void jw_str(json_w *w, const char *s);
void jw_strn(json_w *w, const char *s, size_t n);
void jw_u64(json_w *w, uint64_t v);
void jw_i64(json_w *w, int64_t v);
void jw_bool(json_w *w, bool v);
void jw_null(json_w *w);
void jw_sats(json_w *w, uint64_t sats);                  /* satoshis as a number of BTC with 8 decimals: 12345 -> 0.00012345 */
void jw_hex(json_w *w, const uint8_t *p, size_t n);      /* bytes as a string of lower-case hex */
void jw_raw(json_w *w, const char *json, size_t n);      /* a value already in JSON (the text of another writer), as it is */
/* The text, NUL-terminated (its length in *len when len is not NULL), or NULL after an error or while a bracket is open. It belongs
 * to the writer. */
const char *jw_done(json_w *w, size_t *len);

#endif
