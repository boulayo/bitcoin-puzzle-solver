/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* address.c - Base58Check, bech32/bech32m, SegWit addresses and the Bitcoin address helpers built on them. */
#include <pzcore/address.h>
#include <pzcore/hash.h>

/* ---- Base58Check ---- */

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* value of a Base58 character, or -1 */
static int b58_digit(unsigned char c)
{
    if (c >= '1' && c <= '9') return c - '1';
    if (c >= 'A' && c <= 'H') return c - 'A' + 9;
    if (c >= 'J' && c <= 'N') return c - 'J' + 17;
    if (c >= 'P' && c <= 'Z') return c - 'P' + 22;
    if (c >= 'a' && c <= 'k') return c - 'a' + 33;
    if (c >= 'm' && c <= 'z') return c - 'm' + 44;
    return -1;
}

/* the whole data (payload and checksum) of the longest Base58Check string */
#define B58_RAW_MAX (PZ_BASE58CHECK_MAX_PAYLOAD + 4)

int pz_base58check_decode(const char *s, uint8_t *out, size_t outcap)
{
    if (!s) return -1;
    size_t len = 0;
    while (s[len] && len < PZ_BASE58CHECK_MAX_TEXT) len++;
    if (s[len]) return -1;                                     /* longer than any valid string */
    uint8_t buf[B58_RAW_MAX];   /* the number so far, big-endian, in the last n bytes */
    size_t n = 0, zeros = 0;
    while (zeros < len && s[zeros] == '1') zeros++;
    for (size_t k = zeros; k < len; k++) {
        const int d = b58_digit((unsigned char)s[k]);
        if (d < 0) return -1;
        unsigned carry = (unsigned)d;
        for (size_t i = 0; i < n; i++) {
            const unsigned v = (unsigned)buf[sizeof buf - 1 - i] * 58 + carry;
            buf[sizeof buf - 1 - i] = (uint8_t)v;
            carry = v >> 8;
        }
        while (carry) {
            if (n >= sizeof buf) return -1;
            buf[sizeof buf - 1 - n] = (uint8_t)carry;
            n++;
            carry >>= 8;
        }
    }
    const size_t total = zeros + n;
    if (total < 4 || total - 4 > PZ_BASE58CHECK_MAX_PAYLOAD) return -1;
    const size_t plen = total - 4;
    if (plen > outcap) return -1;
    uint8_t raw[B58_RAW_MAX];
    memset(raw, 0, zeros);
    memcpy(raw + zeros, buf + sizeof buf - n, n);
    uint8_t chk[32];
    pz_sha256d(raw, plen, chk);
    if (memcmp(chk, raw + plen, 4) != 0) return -1;
    if (plen) memcpy(out, raw, plen);
    return (int)plen;
}

int pz_base58check_encode(const uint8_t *payload, size_t len, char *out, size_t outcap)
{
    if (outcap) out[0] = 0;
    if (len > PZ_BASE58CHECK_MAX_PAYLOAD || (len && !payload)) return -1;
    uint8_t raw[B58_RAW_MAX], chk[32];
    if (len) memcpy(raw, payload, len);
    pz_sha256d(raw, len, chk);
    memcpy(raw + len, chk, 4);
    const size_t total = len + 4;
    size_t zeros = 0;
    while (zeros < total && raw[zeros] == 0) zeros++;
    uint8_t digits[B58_RAW_MAX * 138 / 100 + 2];                 /* base-58 digits, least significant first */
    size_t nd = 0;
    for (size_t i = zeros; i < total; i++) {
        unsigned carry = raw[i];
        for (size_t j = 0; j < nd; j++) {
            const unsigned v = (unsigned)digits[j] * 256 + carry;
            digits[j] = (uint8_t)(v % 58);
            carry = v / 58;
        }
        while (carry) { digits[nd++] = (uint8_t)(carry % 58); carry /= 58; }
    }
    const size_t need = zeros + nd;
    if (need + 1 > outcap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < zeros; i++) out[o++] = '1';
    for (size_t i = nd; i > 0; i--) out[o++] = B58[digits[i - 1]];
    out[o] = 0;
    return (int)o;
}

/* ---- bech32 and bech32m ---- */

static const char BECH32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static const int8_t BECH32_REV[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    15, -1, 10, 17, 21, 20, 26, 30,  7,  5, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 29, -1, 24, 13, 25,  9,  8, 23, -1, 18, 22, 31, 27, 19, -1,
     1,  0,  3, 16, 11, 28, 12, 14,  6,  4,  2, -1, -1, -1, -1, -1,
};

#define BECH32_CONST  1u
#define BECH32M_CONST 0x2bc830a3u

static uint32_t bech32_polymod_step(uint32_t chk, unsigned v)
{
    static const uint32_t gen[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    const uint32_t top = chk >> 25;
    chk = (chk & 0x1ffffff) << 5 ^ v;
    for (int i = 0; i < 5; i++)
        if ((top >> i) & 1) chk ^= gen[i];
    return chk;
}

static uint32_t bech32_hrp_polymod(const char *hrp, size_t hrplen)   /* hrp already lower case; the state after the human-readable part */
{
    uint32_t chk = 1;
    for (size_t i = 0; i < hrplen; i++) chk = bech32_polymod_step(chk, (unsigned char)hrp[i] >> 5);
    chk = bech32_polymod_step(chk, 0);
    for (size_t i = 0; i < hrplen; i++) chk = bech32_polymod_step(chk, (unsigned char)hrp[i] & 31);
    return chk;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int pz_bech32_encode(char *out, size_t outcap, const char *hrp, const uint8_t *data, size_t n, pz_bech32_variant variant)
{
    if (outcap) out[0] = 0;
    if (!hrp || (n && !data) || (variant != PZ_BECH32 && variant != PZ_BECH32M)) return -1;
    size_t hl = 0;
    while (hrp[hl] && hl <= PZ_BECH32_MAX_HRP) {
        if ((unsigned char)hrp[hl] < 33 || (unsigned char)hrp[hl] > 126) return -1;
        hl++;
    }
    if (hl == 0 || hl > PZ_BECH32_MAX_HRP) return -1;
    if (hl + 1 + n + 6 > PZ_BECH32_MAX_LEN) return -1;
    if (hl + 1 + n + 6 + 1 > outcap) return -1;
    char lh[PZ_BECH32_MAX_HRP];
    for (size_t i = 0; i < hl; i++) lh[i] = lower(hrp[i]);
    uint32_t chk = bech32_hrp_polymod(lh, hl);
    for (size_t i = 0; i < n; i++) {
        if (data[i] > 31) return -1;
        chk = bech32_polymod_step(chk, data[i]);
    }
    for (int i = 0; i < 6; i++) chk = bech32_polymod_step(chk, 0);
    chk ^= variant == PZ_BECH32 ? BECH32_CONST : BECH32M_CONST;
    size_t o = 0;
    for (size_t i = 0; i < hl; i++) out[o++] = lh[i];
    out[o++] = '1';
    for (size_t i = 0; i < n; i++) out[o++] = BECH32_CHARSET[data[i]];
    for (int i = 0; i < 6; i++) out[o++] = BECH32_CHARSET[(chk >> (5 * (5 - i))) & 31];
    out[o] = 0;
    return (int)o;
}

bool pz_bech32_decode(const char *s, char *hrp, size_t hrpcap, uint8_t *data, size_t *n, pz_bech32_variant *variant)
{
    if (!s || !n || (*n && !data)) return false;
    size_t len = 0;
    bool has_lower = false, has_upper = false;
    while (s[len]) {
        const unsigned char c = (unsigned char)s[len];
        if (len >= PZ_BECH32_MAX_LEN || c < 33 || c > 126) return false;
        if (c >= 'a' && c <= 'z') has_lower = true;
        if (c >= 'A' && c <= 'Z') has_upper = true;
        len++;
    }
    if (len < 8 || (has_lower && has_upper)) return false;
    size_t sep = len;                                  /* the last '1' separates the human-readable part */
    while (sep > 0 && s[sep - 1] != '1') sep--;
    if (sep < 2) return false;                          /* no separator, or an empty human-readable part */
    sep--;
    const size_t hl = sep, dl = len - sep - 1;         /* characters of hrp, of data and checksum */
    if (hl > PZ_BECH32_MAX_HRP || dl < 6) return false;
    if (hrp && hl + 1 > hrpcap) return false;
    const size_t nd = dl - 6;
    if (nd > *n) return false;
    char lh[PZ_BECH32_MAX_HRP];
    for (size_t i = 0; i < hl; i++) lh[i] = lower(s[i]);
    uint32_t chk = bech32_hrp_polymod(lh, hl);
    uint8_t vals[PZ_BECH32_MAX_LEN];
    for (size_t i = 0; i < dl; i++) {
        const unsigned char c = (unsigned char)lower(s[sep + 1 + i]);
        const int v = BECH32_REV[c];                   /* c < 128 here: the loop above rejected anything above 126 */
        if (v < 0) return false;
        vals[i] = (uint8_t)v;
        chk = bech32_polymod_step(chk, (unsigned)v);
    }
    pz_bech32_variant var;
    if (chk == BECH32_CONST) var = PZ_BECH32;
    else if (chk == BECH32M_CONST) var = PZ_BECH32M;
    else return false;
    if (hrp) { memcpy(hrp, lh, hl); hrp[hl] = 0; }
    if (nd) memcpy(data, vals, nd);
    *n = nd;
    if (variant) *variant = var;
    return true;
}

bool pz_convert_bits(uint8_t *out, size_t *outlen, unsigned tobits, const uint8_t *in, size_t inlen, unsigned frombits, bool pad)
{
    if (!outlen || (inlen && !in) || frombits < 1 || frombits > 8 || tobits < 1 || tobits > 8) return false;
    uint32_t acc = 0;
    unsigned bits = 0;
    const uint32_t maxv = (1u << tobits) - 1, max_acc = (1u << (frombits + tobits - 1)) - 1;
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        if (in[i] >> frombits) return false;
        acc = ((acc << frombits) | in[i]) & max_acc;
        bits += frombits;
        while (bits >= tobits) {
            bits -= tobits;
            if (o >= *outlen) return false;
            out[o++] = (uint8_t)((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits) {
            if (o >= *outlen) return false;
            out[o++] = (uint8_t)((acc << (tobits - bits)) & maxv);
        }
    } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
        return false;
    }
    *outlen = o;
    return true;
}

/* ---- SegWit addresses ---- */

int pz_segwit_address_encode(char *out, size_t outcap, const char *hrp, unsigned witver, const uint8_t *prog, size_t proglen)
{
    if (outcap) out[0] = 0;
    if (witver > 16 || !prog || proglen < 2 || proglen > 40) return -1;
    if (witver == 0 && proglen != 20 && proglen != 32) return -1;
    uint8_t data[1 + 64];
    size_t nd = sizeof data - 1;
    data[0] = (uint8_t)witver;
    if (!pz_convert_bits(data + 1, &nd, 5, prog, proglen, 8, true)) return -1;
    return pz_bech32_encode(out, outcap, hrp, data, nd + 1, witver == 0 ? PZ_BECH32 : PZ_BECH32M);
}

bool pz_segwit_address_decode(const char *hrp, const char *addr, unsigned *witver, uint8_t *prog, size_t *proglen)
{
    if (!hrp || !addr || !proglen || !witver || (*proglen && !prog)) return false;
    char got[PZ_BECH32_MAX_HRP + 1];
    uint8_t data[PZ_BECH32_MAX_LEN];
    size_t nd = sizeof data;
    pz_bech32_variant var;
    if (!pz_bech32_decode(addr, got, sizeof got, data, &nd, &var) || nd < 1) return false;
    size_t i = 0;
    for (; got[i] && hrp[i]; i++)
        if (got[i] != lower(hrp[i])) return false;
    if (got[i] || hrp[i]) return false;
    const unsigned v = data[0];
    if (v > 16) return false;
    if ((v == 0) != (var == PZ_BECH32)) return false;       /* version 0 uses bech32, the others bech32m */
    uint8_t p[40];
    size_t pl = sizeof p;
    if (!pz_convert_bits(p, &pl, 8, data + 1, nd - 1, 5, false)) return false;
    if (pl < 2 || pl > 40 || (v == 0 && pl != 20 && pl != 32)) return false;
    if (pl > *proglen) return false;
    memcpy(prog, p, pl);
    *proglen = pl;
    *witver = v;
    return true;
}

/* ---- Bitcoin addresses and keys ---- */

bool pz_address_to_hash160(const char *addr, uint8_t h160[20])
{
    if (!addr) return false;
    if ((addr[0] == 'b' || addr[0] == 'B') && (addr[1] == 'c' || addr[1] == 'C')) {
        uint8_t prog[40];
        size_t pl = sizeof prog;
        unsigned v;
        if (!pz_segwit_address_decode("bc", addr, &v, prog, &pl) || v != 0 || pl != 20) return false;
        memcpy(h160, prog, 20);
        return true;
    }
    uint8_t payload[PZ_BASE58CHECK_MAX_PAYLOAD];
    const int n = pz_base58check_decode(addr, payload, sizeof payload);
    if (n != 21 || payload[0] != 0x00) return false;
    memcpy(h160, payload + 1, 20);
    return true;
}

bool pz_hash160_to_address(const uint8_t h160[20], char *out, size_t outcap)
{
    uint8_t payload[21];
    payload[0] = 0;
    memcpy(payload + 1, h160, 20);
    return pz_base58check_encode(payload, 21, out, outcap) > 0;
}

int pz_hash160_to_segwit_address(const char *hrp, const uint8_t h160[20], char *out, size_t outcap)
{
    return pz_segwit_address_encode(out, outcap, hrp, 0, h160, 20);
}

bool pz_privkey_to_wif(const uint8_t key_be[32], bool compressed, char *out, size_t outcap)
{
    uint8_t payload[34];
    payload[0] = 0x80;
    memcpy(payload + 1, key_be, 32);
    size_t n = 33;
    if (compressed) payload[n++] = 1;
    return pz_base58check_encode(payload, n, out, outcap) > 0;
}
