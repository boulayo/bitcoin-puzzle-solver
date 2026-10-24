/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* doctor.h - 'pzsolve doctor': one page that checks the program, the machine and the saved state and says what is wrong. */
#ifndef PZ_DOCTOR_H
#define PZ_DOCTOR_H

#include "common.h"

int doctor_run(int argc, char **argv);
/* One line of the report: level 'i' information, 'o' fine, 'w' warning, 'x' error (the last two are counted). */
void doctor_line(char level, const char *fmt, ...);

#endif
