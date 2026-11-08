/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* noise.h - Noise_XX_secp256k1_ChaChaPoly_SHA256 (the Noise Protocol Framework, revision 34) and the frames of the channel after it, as in
 * BOLT 8: a 2-byte length and the body, each encrypted with its own nonce, keys rotated every 1000 encryptions (docs/en/NETWORK.md 7.1).
 *
 * A state machine with no I/O: the caller moves the bytes. DH is libsecp256k1 ECDH, whose output is SHA256 of the compressed shared point;
 * public keys travel compressed (33 bytes). The ephemeral secret is given by the caller, so a handshake can be replayed in a test. */
#ifndef PZPEER_NOISE_H
#define PZPEER_NOISE_H

#include <secp256k1.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOISE_PAYLOAD_MAX 256      /* the payload of a handshake message */
#define NOISE_HS_MAX (33 + 49 + NOISE_PAYLOAD_MAX + 16)
#define NOISE_FRAME_MAX 65535      /* the body of a frame */
#define NOISE_HEAD 18              /* the encrypted length of a frame */
#define NOISE_TAG 16

typedef struct {
    uint8_t k[32], ck[32];
    uint64_t n;
} noise_cs;                         /* one direction of the channel */

typedef struct {
    const secp256k1_context *ctx;
    bool initiator, has_k;
    int msg;                        /* the next message of the pattern (0: -> e; 1: <- e, ee, s, es; 2: -> s, se); 3 when done; -1 broken */
    uint8_t h[32], ck[32], k[32];
    uint64_t n;
    uint8_t s[32], spub[33], e[32], epub[33], rs[33], re[33];
} noise_hs;

/* False for a static or ephemeral secret that is not a valid key. The prologue binds the network (docs/en/NETWORK.md 7.1). */
bool noise_hs_init(noise_hs *hs, const secp256k1_context *ctx, bool initiator, const uint8_t *prologue, size_t plen, const uint8_t s[32],
                   const uint8_t e[32]);
/* The next message, with payload (at most NOISE_PAYLOAD_MAX bytes), into out (NOISE_HS_MAX bytes). Its length, 0 when it is not this side's turn. */
size_t noise_hs_write(noise_hs *hs, const uint8_t *payload, size_t plen, uint8_t *out);
/* Reads the next message of the other side; its payload into payload (NOISE_PAYLOAD_MAX bytes), its length in *plen. False when the message is
 * malformed or does not authenticate: the handshake is then broken. */
bool noise_hs_read(noise_hs *hs, const uint8_t *in, size_t len, uint8_t *payload, size_t *plen);
/* After the third message: the two directions, the static key of the other side and the hash of the handshake (the same on both sides: a
 * session can sign it). Wipes the handshake. */
bool noise_hs_split(noise_hs *hs, noise_cs *tx, noise_cs *rx, uint8_t remote_static[33], uint8_t hash[32]);

/* A frame: body (at most NOISE_FRAME_MAX bytes) into out, NOISE_HEAD + len + NOISE_TAG bytes. */
size_t noise_frame_seal(noise_cs *tx, const uint8_t *body, size_t len, uint8_t *out);
/* The length of the body that follows the head (NOISE_HEAD bytes); false when the head does not authenticate. */
bool noise_frame_head(noise_cs *rx, const uint8_t head[NOISE_HEAD], size_t *len);
/* The body (len + NOISE_TAG bytes) into body; false when it does not authenticate. in and body may be the same buffer. */
bool noise_frame_body(noise_cs *rx, const uint8_t *in, size_t len, uint8_t *body);

#endif
