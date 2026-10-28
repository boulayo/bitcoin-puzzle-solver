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
/* The state to show for a campaign, given the one its files say (`written`: "RUNNING", "STOPPED", "COMPLETED"...), whether FOUND.txt
 * exists and whether some process holds the lock. The files say RUNNING until the next write, so a process that was killed leaves
 * it behind: without the lock the state is STOPPED. 'list' and 'info' both use it. Returns `written` itself or a constant. */
const char *campaign_shown_state(const char *written, bool found, bool running);
/* The text of report.txt of the campaign in `dir`, with its "State:" line corrected by campaign_shown_state; NULL when there is no
 * report. Free with free(). */
char *campaign_report(const char *dir);
/* Prints a table of every campaign found under `base`; returns the number of campaigns. */
int campaigns_list(const char *base);

#endif
