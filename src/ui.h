/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ui.h - text rendering of the campaign status (terminal dashboard, one-line summary). */
#ifndef PZ_UI_H
#define PZ_UI_H

#include "run.h"

/* Total, CPU and GPU smoothed rates; *ncpu is the number of CPU workers. */
double ui_rates(const run_ctx *r, double *cpu, double *gpu, int *ncpu);
const char *ui_state_name(const run_ctx *r, bool final);

char *ui_target_tag(const run_ctx *r, char *buf, size_t n);  /* "#71", or the campaign name for a custom target */
char *ui_fmt_count(long double v, char *buf, size_t n);          /* 1.23 M / 4.56e+18 */
char *ui_fmt_eta(double secs, char *buf, size_t n);              /* "n/a" for unknown/infinite */
char *ui_fmt_range_bits(const pz_u256 *sz, char *buf, size_t n);  /* "2^74" for an exact power of two, otherwise "~2^21.0" */
char *ui_fmt_prob(double p, char *buf, size_t n);                /* "12.34%", "3.2e-07%" */

/* Multi-line dashboard (lines joined by '\n', no trailing newline); returns the number of lines. */
int ui_render_full(const run_ctx *r, const progress_info *pi, const eta_summary *es, bool final, int cols, bool color, char *buf, size_t n);
/* One compact line (no newline). */
void ui_render_line(const run_ctx *r, const progress_info *pi, const eta_summary *es, int cols, char *buf, size_t n);
/* The line of the end of a session: the state and the progress; the completion times only for a campaign that can go on. At most
 * `cols` - 1 columns (cols == 0: no terminal, never shortened). */
void ui_render_final_line(const run_ctx *r, const progress_info *pi, const eta_summary *es, int cols, char *buf, size_t n);

#endif
