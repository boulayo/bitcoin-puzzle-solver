/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* outfmt.h - the look shared by the check lists and the tables that the commands print (selftest, bench, ...). */
#pragma once
#include "common.h"
#include <stdarg.h>

#define PZ_LABEL_COL 41     /* width of the label of a check-list line: the results line up in one column (column 44) */

/* "  label    result": one line of a check list. */
static inline void pz_result_line(const char *label, const char *fmt, ...) __attribute__((format(PZ_PRINTF, 2, 3)));
static inline void pz_result_line(const char *label, const char *fmt, ...)
{
    char res[512], line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res, sizeof res, fmt, ap);
    va_end(ap);
    if ((int)strlen(label) > PZ_LABEL_COL) {                      /* a long label: the result below it, in the results column */
        printf("  %s\n", label);
        label = "";
    }
    snprintf(line, sizeof line, "  %-*s %s", PZ_LABEL_COL, label, res);
    pz_fputs_wrap(stdout, line, PZ_LINE_MAX, 3 + PZ_LABEL_COL);   /* a long result continues under the results column */
    putchar('\n');
}

/* The same line in two steps, for a check that takes a while: the label first, the result when it is known. */
static inline void pz_result_begin(const char *label)
{
    if ((int)strlen(label) > PZ_LABEL_COL) printf("  %s\n%*s ", label, PZ_LABEL_COL + 2, "");
    else printf("  %-*s ", PZ_LABEL_COL, label);
    fflush(stdout);
}

/* A rate in a fixed-width cell of a table: the value right-aligned with two decimals and the multiplier in a column of its own
 * ("  75.25 M", "   0.00  "), so that the numbers of a column line up whatever their size. */
static inline char *pz_fmt_cell(double v, char *buf, size_t n)
{
    static const char suf[] = " kMGTPE";
    int i = 0;
    while (v >= 1000.0 && i < 6) { v /= 1000.0; i++; }
    snprintf(buf, n, "%6.2f %c", v, suf[i]);
    return buf;
}

/* "== Title": the heading of a section of the output of a command, after a blank line (none before the first one of the process;
 * the counter lives in selftest.c). */
extern int pz_sections_printed;
static inline void pz_section(const char *title)
{
    printf("%s== %s\n", pz_sections_printed++ ? "\n" : "", title);
}

#define PZ_PROP_COL 20      /* width of the name of a property line */

/* "  name              text": one property of an item (a GPU, a campaign); long text is wrapped under the first line. */
static inline void pz_prop_line(const char *name, const char *fmt, ...) __attribute__((format(PZ_PRINTF, 2, 3)));
static inline void pz_prop_line(const char *name, const char *fmt, ...)
{
    char text[900], line[1000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "  %-*s %s", PZ_PROP_COL, name, text);
    pz_fputs_wrap(stdout, line, PZ_LINE_MAX, 3 + PZ_PROP_COL);
    putchar('\n');
}
