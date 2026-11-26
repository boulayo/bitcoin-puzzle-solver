/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* canary.h - the canary of the silent misses of the linear search.
 *
 * Every key is a near miss (word SCAN_FILTER_WORD of its hash160 is the target's: bytes 8 to 11) with probability 2^-32, and the engines
 * and kernels report every near miss: the near misses a worker found in the K keys it scanned are a sum of K independent trials of mean
 * 2^-32, and a worker that skips keys finds fewer. The workers are the CPU engine (one canary for its threads, which all run it) and each
 * GPU. The keys a worker marks as scanned go into windows of CANARY_WINDOW keys (64 near misses expected); at the end of each one the near
 * misses of the window and those of the whole session are set against what was expected, and a count whose probability is below
 * e^CANARY_LOG_P (1e-9, from the Chernoff bound, an upper bound of the probability: a correct worker trips it less often than that)
 * condemns the worker. Its keys go back to be scanned again: those of the window when the window failed, everything the campaign
 * marked as scanned in this session when the session failed (a worker that misses a small share of the keys is seen only over many
 * windows, and the earlier ones cannot be told apart from those of the other workers), and every claim it completes after that. The
 * worker must then stop. A window takes 2^38 keys: about 90 s of a GPU at 3 G keys/s, 45 min of a CPU at 100 M keys/s; a session that
 * ends sooner judges nothing. The kangaroo and BSGS have no near misses: the canary is the linear search's. */
#ifndef PZ_CANARY_H
#define PZ_CANARY_H

#include "jobq.h"
#include <stdatomic.h>

#define CANARY_WINDOW ((u64)1 << 38)
#define CANARY_LOG_P (-20.723265836946411)   /* ln 1e-9 */

typedef struct {
    pthread_mutex_t mu;
    char name[160];                   /* the worker, as the log names it: "the CPU engine avx512i", "GPU[0] NVIDIA RTX A2000" */
    u64 keys, near;                   /* the session */
    u64 wkeys, wnear;                 /* the window under way */
    struct { u128 v; u64 n; } *claim; /* its claims: virtual position and keys */
    size_t nclaim, capclaim;
    _Atomic bool condemned;           /* read without the lock by those who would use the worker */
} canary;

void canary_init(canary *c, const char *name);
void canary_free(canary *c);
/* ln of an upper bound of P(X <= n) for X a sum of independent trials of mean lambda: -lambda (1 - a + a ln a), a = n / lambda (0 for
 * n >= lambda). */
double canary_log_tail(double lambda, u64 n);
/* A claim of the worker of c done: covered of its keys scanned (sched_complete), `near` near misses found among them. Marks them as scanned
 * in s and, at the end of a window, judges the worker (above). False when the worker is condemned, then or before (its claim is given
 * back): it must stop. */
bool canary_complete(canary *c, sched_t *s, const claim_t *cl, u64 covered, u64 near);

#endif
