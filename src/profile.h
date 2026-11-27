/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* profile.h - the best CPU/GPU combination of this machine, measured once and remembered (first-run calibration). */
#ifndef PZ_PROFILE_H
#define PZ_PROFILE_H

#include "run.h"

/* Before a campaign starts: unless -t, --gpu, --no-cpu, --no-gpu were given (or PZ_NO_PROFILE is set), looks up the saved profile for
 * this program version and hardware, measuring CPU and GPUs together on the first run (about two minutes), and applies it
 * to `r` (thread count and GPU list). `force` measures again. Does nothing without a usable GPU. */
void profile_apply(run_ctx *r, bool force);
/* 'doctor': the saved profile entries of this program version. */
void profile_doctor(void);

#endif
