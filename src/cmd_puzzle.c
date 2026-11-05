/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_puzzle.c - 'pzsolve puzzle' and 'pzsolve puzzles': the built-in database, and the effort a puzzle takes at the speed of this
 * machine or at the one given. */
#include "cmd.h"
#include "selftest.h"
#include "kangaroo.h"
#include "puzzles.h"
#include "bsgs.h"
#include "platform.h"
#include "outfmt.h"
#include <math.h>

/* A speed such as 800000000, 800M or 1.5G (suffixes k, M, G, T). */
static double opt_rate(const char *opt, const char *s)
{
    double v;
    if (!pz_parse_rate(s, &v)) pz_die("%s: invalid value '%s' (expected a speed such as 800M or 1.5G)", opt, s);
    return v;
}

/* The method to suggest for an open puzzle. */
static const char *recommended_method(const puzzle_info *p)
{
    if (!p->pubkey[0]) return "linear search (only the address is known)";
    return "kangaroo (public key known)";
}

static void print_puzzle_effort(const puzzle_info *p, double rate_lin, double rate_kang, bool measured)
{
    const int n = p->n;
    char linear_rate[32], kangaroo_rate[32], time_50[48], time_100[48], time_90[48];
    if (rate_lin > 0)
        pz_prop_line("speed used", "%s keys/s (linear)%s%s%s; %s", pz_fmt_si(rate_lin, linear_rate, sizeof linear_rate), rate_kang > 0 && p->pubkey[0] ? ", " : "",
                     p->pubkey[0] && rate_kang > 0 ? pz_fmt_si(rate_kang, kangaroo_rate, sizeof kangaroo_rate) : "", p->pubkey[0] && rate_kang > 0 ? " steps/s (kangaroo)" : "",
                     measured ? "measured now on the CPU of this machine" : "given with --rate");
    pz_section("Mean effort");
    const long double W = ldexpl(1.0L, n - 1);
    pz_prop_line("linear search", "%.3Le keys on average (2^%d), %.3Le in the worst case", W / 2, n - 2, W);
    if (rate_lin > 0)
        pz_prop_line("", "time: 50%% in %s, 100%% in %s", pz_fmt_duration((double)(W / 2) / rate_lin, time_50, sizeof time_50), pz_fmt_duration((double)W / rate_lin, time_100, sizeof time_100));
    if (p->pubkey[0]) {
        long double st = KANG_C_MEAN * sqrtl(W);
        pz_prop_line("kangaroo", "%.3Le steps on average (2^%.1f)", st, (double)log2l(st));
        if (rate_kang > 0)
            pz_prop_line("", "time: 50%% in %s, 90%% in %s", pz_fmt_duration((double)(0.9395L * KANG_C_MEAN * sqrtl(W)) / rate_kang, time_50, sizeof time_50),
                         pz_fmt_duration((double)(1.7122L * KANG_C_MEAN * sqrtl(W)) / rate_kang, time_90, sizeof time_90));
        /* BSGS with a RAM table, if one that beats the kangaroo fits in 70% of the free memory */
        sysinfo_t si;
        sysinfo_detect(&si);
        pz_u256 w256 = pz_u256_pow2((unsigned)(n - 1));
        bool viable;
        u64 T = bsgs_choose_T(&w256, (u64)(0.70 * (double)si.mem_avail), &viable);
        if (viable) {
            char mb[48], cs[48];
            pz_fmt_bytes((double)bsgs_bytes_for(T), mb, sizeof mb);
            long double cost = bsgs_mean_cost(&w256, T);
            char tg[32];
            pz_prop_line("BSGS in RAM", "table of %s entries (%s), %.3Le operations on average (2^%.1f), deterministic", pz_fmt_u64(T, tg, sizeof tg), mb, cost, (double)log2l(cost));
            if (rate_kang > 0) pz_prop_line("", "time, at the kangaroo speed: %s on average", pz_fmt_duration((double)cost / rate_kang, cs, sizeof cs));
        } else {
            pz_prop_line("BSGS in RAM", "not worthwhile: the best table does not fit in 70%% of the free RAM of this machine");
        }
    }
}

int cmd_puzzle(int argc, char **argv)
{
    if (argc < 3) pz_usage_error("puzzle", "the number of the puzzle is missing");
    int n = (int)pz_arg_int("puzzle", argv[2], 1, 256);
    double rate_lin = -1, rate_kang = -1;
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--rate")) {
            if (i + 1 >= argc) pz_usage_error("puzzle", "the option '--rate' needs a value");
            rate_lin = rate_kang = opt_rate("--rate", argv[++i]);
        }
        else pz_usage_error("puzzle", "unrecognized option '%s'", argv[i]);
    pz_u256 a, b;
    if (!puzzle_range(n, &a, &b)) pz_die("puzzle out of range (1..256)");
    char ha[80], hb[80], ds[100];
    pz_u256_to_hex(&a, ha);
    pz_u256_to_hex(&b, hb);
    pz_u256 sz = cfg_range_size(&(campaign_cfg){.range_start = a, .range_end = b});
    pz_u256_to_dec(&sz, ds);
    const puzzle_info *p = puzzle_find(n);
    char title[32];
    snprintf(title, sizeof title, "Puzzle #%d", n);
    pz_section(title);
    if (p) {
        pz_prop_line("address", "%s", p->address);
        pz_prop_line("public key", "%s", p->pubkey[0] ? p->pubkey : "not known (it never appeared on the blockchain)");
        if (p->privkey[0]) pz_prop_line("status", "solved, the key is 0x%s", p->privkey);
        else pz_prop_line("status", "open in the built-in database: check that the funds are still on the address");
    } else if (puzzle_retired_address(n)) {
        pz_prop_line("address", "%s", puzzle_retired_address(n));
        pz_prop_line("public key", "not known");
        pz_prop_line("status", "emptied: the creator withdrew the funds in 2017 (block 475240), together with all puzzles 161..256");
    }
    char dsg[96];
    pz_prop_line("range", "0x%s - 0x%s", ha, hb);
    pz_prop_line("keys", "%s (2^%d)", pz_group_digits(ds, dsg, sizeof dsg), n - 1);
    if (p) {
        const bool solved = p->privkey[0] != 0;
        if (!solved) pz_prop_line("best method", "%s", recommended_method(p));
        bool measured = rate_lin < 0;
        if (measured) {
            const bool tty = pz_stderr_is_terminal();
            fprintf(stderr, tty ? "measuring the speed of this machine (about 4 seconds)...\r" : "measuring the speed of this machine (about 4 seconds)...\n");
            rate_lin = bench_cpu_rate(1.0);
            rate_kang = p->pubkey[0] ? kangaroo_bench(2.0, false) : 0;
            if (tty) fprintf(stderr, "\x1b[K");
        }
        print_puzzle_effort(p, rate_lin, rate_kang, measured);
        printf("\n");
        if (measured) printf("GPUs, if present, add to the CPU: give the overall speed with --rate.\n");
        if (solved) printf("A test run (the key is known): pzsolve hunt -p %d\n", n);
        else printf("To start the campaign: pzsolve hunt -p %d\n", n);
    } else {
        long double w = ldexpl(1.0L, n - 1);          /* emptied: no public key, so only the linear search */
        pz_section("Mean effort");
        pz_prop_line("linear search", "%.3Le keys on average (2^%d), %.3Le in the worst case", w / 2, n - 2, w);
    }
    return 0;
}

int cmd_puzzles(int argc, char **argv)
{
    bool only_open = false, only_solved = false, only_pub = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--open")) only_open = true;
        else if (!strcmp(argv[i], "--solved")) only_solved = true;
        else if (!strcmp(argv[i], "--pubkey")) only_pub = true;
        else pz_usage_error("puzzles", "unrecognized option '%s'", argv[i]);
    }
    if (only_open && only_solved) pz_die("--open and --solved are mutually exclusive");
    printf("%4s  %-9s %-6s %-36s %s\n", "N", "status", "pubkey", "address", "mean effort");
    int shown = 0, nsolved = 0, nopen = 0;
    for (int i = 0; i < PZ_NUM_PUZZLES; i++) {
        const puzzle_info *p = &PZ_PUZZLES[i];
        bool solved = p->privkey[0] != 0;
        if (solved) nsolved++; else nopen++;
        if ((only_open && solved) || (only_solved && !solved) || (only_pub && !p->pubkey[0])) continue;
        double eff = p->pubkey[0] ? (p->n - 1) / 2.0 + log2(KANG_C_MEAN) : (p->n - 2);
        printf("%4d  %-9s %-6s %-36s 2^%.1f %s\n", p->n, solved ? "solved" : "OPEN", p->pubkey[0] ? "yes" : "no", p->address, eff,
               p->pubkey[0] ? "steps" : "keys");             /* steps of the kangaroo, keys of the linear search */
        shown++;
    }
    char tail[300];
    snprintf(tail, sizeof tail, "%d of %d puzzles (%d solved, %d open). Mean effort: kangaroo steps with a known public key, linear-search keys "
             "without. Puzzles 161..256 (emptied in 2017): 'pzsolve puzzle N'.\n", shown, PZ_NUM_PUZZLES, nsolved, nopen);
    putchar('\n');
    pz_fputs_wrap(stdout, tail, PZ_LINE_MAX, 0);
    return 0;
}
