/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* signer.h - one pzfirma process for one signing session (docs/en/NETWORK.md 7.6): started with "session", spoken to with frames of a
 * 4-byte big-endian length and a type byte on its stdin and stdout. pzpeer relays the opaque messages (0x81 out, 0x02 in) to the partner. */
#ifndef PZPEER_SIGNER_H
#define PZPEER_SIGNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SIGNER_FRAME_MAX (1u << 20)     /* the limit of pzfirma */

enum { SIGNER_JOB = 0x01, SIGNER_IN = 0x02, SIGNER_OUT = 0x81, SIGNER_RESULT = 0x82, SIGNER_ERROR = 0x83 };

typedef struct {
    pid_t pid;
    int to, from;                       /* its stdin and stdout */
} signer;

/* Starts `path session`; its stderr is that of pzpeer. */
bool signer_start(signer *s, const char *path);
bool signer_send(signer *s, uint8_t type, const uint8_t *payload, size_t len);
/* The next frame; *payload is malloc'ed (len + 1 bytes, NUL after the payload) and is the caller's to free. False at the end of its output,
 * on a malformed frame, or when nothing comes within timeout_ms (the caller then stops the process). */
bool signer_recv(signer *s, uint8_t *type, uint8_t **payload, size_t *len, int timeout_ms);
/* Kills the process and nothing else: a thread blocked writing to it (it stopped reading) gets an error. */
void signer_kill(signer *s);
/* Ends the process (killed if still running) and closes the pipes. */
void signer_stop(signer *s);
/* `path primes file`: the primes of a session, made ahead (seconds to a minute of CPU). True when it ended well. */
bool signer_primes(const char *path, const char *file);

/* The JSON job of a session: party i, session id, own piece, the two public pieces and the key (compressed), the primes file and the digests.
 * malloc'ed; it holds the secret piece, so the caller wipes it (pz_secure_zero over its length) before free. NULL for a primes path that
 * would need escaping. */
char *signer_job(int i, const uint8_t eid[32], const uint8_t share[32], const uint8_t pub[2][33], const uint8_t key[33], const char *primes,
                 const uint8_t (*digests)[32], int ndigests);

#endif
