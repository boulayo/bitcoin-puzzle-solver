/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* linear.h - the home of a shared linear search (docs/en/NETWORK.md 11), for a puzzle whose public key is not known: it picks a block of
 * the range that nobody claimed, has pzsolve scan it with its near misses, and publishes the claim of the block with its proof keys; a
 * node of the table that keeps the claims of others. A key found stays with the home that found it: nothing is sent, nothing is shared. */
#ifndef PZPEER_LINEAR_H
#define PZPEER_LINEAR_H

#include "home.h"
#include "netfile.h"

/* Runs until SIGINT or SIGTERM, or until the key is found (with exit_when_done, also at the end of the first block). dir: the folder of
 * the home, made private. 0 when it ended well. */
int linear_run(const home_cfg *cfg, const netfile *nf, const char *dir);

#endif
