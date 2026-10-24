/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* campaigns.h - inspection of the campaign directories ('pzsolve list') and the per-campaign lock. */
#ifndef PZ_CAMPAIGNS_H
#define PZ_CAMPAIGNS_H

#include "common.h"

/* Takes the exclusive lock of a campaign directory (held until the process exits); dies with a clear message
 * when another process already runs the campaign. Returns the descriptor holding the lock. */
int campaign_lock(const char *dir);
/* True when some process currently holds the campaign lock. */
bool campaign_is_running(const char *dir);
/* Prints a table of every campaign found under `base`; returns the number of campaigns. */
int campaigns_list(const char *base);

#endif
