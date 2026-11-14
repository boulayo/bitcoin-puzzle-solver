/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* signer.h - one pzsign process for one signing session (docs/en/NETWORK.md 7.6): started with "session", spoken to with frames of a
 * 4-byte big-endian length and a type byte on its stdin and stdout. pzpeer relays the opaque messages (0x81 out, 0x02 in) to the partner. */
#ifndef PZPEER_SIGNER_H
#define PZPEER_SIGNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sys.h"

#define SIGNER_FRAME_MAX (1u << 20)     /* the limit of pzsign */

enum { SIGNER_JOB = 0x01, SIGNER_IN = 0x02, SIGNER_OUT = 0x81, SIGNER_RESULT = 0x82, SIGNER_ERROR = 0x83, SIGNER_UNPAID = 0x84 };

typedef struct {
    sys_proc proc;
    int to, from;                       /* its stdin and stdout */
} signer;

/* Starts `path session`; its stderr is that of pzpeer. */
bool signer_start(signer *s, const char *path);
bool signer_send(signer *s, uint8_t type, const uint8_t *payload, size_t len);
/* Closes its input: at the end of its input pzsign gives its answer. */
void signer_end_input(signer *s);
/* The next frame; *payload is malloc'ed (len + 1 bytes, NUL after the payload) and is the caller's to free. False at the end of its output,
 * on a malformed frame, or when nothing comes within timeout_ms (the caller then stops the process). */
bool signer_recv(signer *s, uint8_t *type, uint8_t **payload, size_t *len, int timeout_ms);
/* Kills the process and nothing else: a thread blocked writing to it (it stopped reading) gets an error. */
void signer_kill(signer *s);
/* Ends the process (killed if still running) and closes the pipes. */
void signer_stop(signer *s);
#define SIGNER_DIGESTS_MAX 64
#define SIGNER_JOB_MAX (166 + 32 * SIGNER_DIGESTS_MAX)

/* The job of a session (signer/README.md): party i, the party that gets the signatures first (and gives them to the other: S, where the
 * `first` of payout_terms is F), the session
 * id, the own piece, the two public pieces and the key (compressed), and 1 to SIGNER_DIGESTS_MAX digests. Written to out; returns its
 * length. It holds the secret piece: the caller wipes it (pz_secure_zero) once sent. The answer (0x82) is r || s of each digest. */
size_t signer_job(uint8_t out[SIGNER_JOB_MAX], int i, int first, const uint8_t eid[32], const uint8_t share[32], const uint8_t pub[2][33],
                  const uint8_t key[33], const uint8_t (*digests)[32], int ndigests);

#endif
