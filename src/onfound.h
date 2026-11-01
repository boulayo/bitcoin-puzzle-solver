/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* onfound.h - the command of `hunt --on-found "COMMAND"`: run once, in a thread of its own, when the key has been found and FOUND.txt written. */
#ifndef PZ_ONFOUND_H
#define PZ_ONFOUND_H

#include "common.h"

#define ONFOUND_TIMEOUT 120.0     /* seconds the command is given */

/* Starts the command with the shell of the platform and, in its environment, PZ_FOUND_SEALED (the PZENC1 line; empty when the key is not encrypted),
 * PZ_FOUND_ADDRESS, PZ_FOUND_PUZZLE (0 for none), PZ_FOUND_CAMPAIGN and PZ_FOUND_FILE (the path of FOUND.txt). Never the key itself. Returns at
 * once; only the first call does anything. */
void onfound_start(const char *cmd, const char *sealed, const char *address, int puzzle, const char *campaign, const char *found_file);
/* Waits for the command to end (it is stopped after ONFOUND_TIMEOUT seconds from its start) and logs how it ended. Does nothing when none was
 * started. Called when the final box is on the screen: the search was not held up by the command, but the program does not end before it. */
void onfound_wait(void);

#endif
