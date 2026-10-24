/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* profile.h - the best CPU/GPU combination of this machine, measured once and remembered (first-run calibration). */
#ifndef PZ_PROFILE_H
#define PZ_PROFILE_H

#include "run.h"

/* Before a campaign starts: unless -t, --gpu, --no-cpu, --no-gpu were given (or `disable`), looks up the saved profile for
 * this program version and hardware, measuring CPU and GPUs together on the first run (about two minutes), and applies it
 * to `r` (thread count and GPU list). `force` measures again. Does nothing without a usable GPU. */
void profile_apply(run_ctx *r, bool force, bool disable);
/* After profile_apply and the final choice of threads and GPUs: finds the first guess of the rate for the completion times (the steady rate
 * an earlier run of this very configuration reached, else the calibration's) and arranges for this run's steady rate to be remembered.
 * Not called with --no-tune. */
void profile_rate_setup(run_ctx *r);
/* 'doctor': the saved profile entries of this program version. */
void profile_doctor(void);

#endif
