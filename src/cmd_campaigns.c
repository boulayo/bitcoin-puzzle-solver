/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_campaigns.c - the commands about existing campaigns and their shares: 'info', 'list', 'where' and 'split'. */
#include "cmd.h"
#include "campaigns.h"
#include "outfmt.h"

int cmd_info(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    if (!o.campaign[0] && o.puzzle) snprintf(o.campaign, sizeof o.campaign, "p%d", o.puzzle);
    if (!o.campaign[0]) pz_die("specify the campaign with -c NAME or -p N");
    char p[700];
    snprintf(p, sizeof p, "%s/%s", o.dir, o.campaign);
    char *t = campaign_report(p);
    if (t) {
        /* report.txt is written for any width: on a terminal each line is wrapped under its value (column 16) */
        for (char *q = t; *q;) {
            char *nl = strchr(q, '\n');
            if (nl) *nl = 0;
            pz_fputs_wrap(stdout, q, PZ_LINE_MAX, 16);
            putchar('\n');
            q = nl ? nl + 1 : q + strlen(q);
        }
        free(t);
        return 0;
    }
    snprintf(p, sizeof p, "%s/%s/campaign.conf", o.dir, o.campaign);
    t = pz_read_file(p, NULL);
    if (!t) pz_die("campaign '%s' not found in %s", o.campaign, o.dir);
    printf("(no report yet: the campaign has never been started)\n\n%s", t);
    free(t);
    return 0;
}

int cmd_list(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    campaigns_list(o.dir);
    return 0;
}

/* 'where': whether the key at virtual position local:off of this computer's order is already behind the scan, and when the scan gets there
 * at the speed of the last session (status.json). Nothing when the state cannot be read. */
static void where_state(sched_t *s, const char *cdir, const pz_u256 *local, u64 off, const char *unit)
{
    char p[800], err[256];
    snprintf(p, sizeof p, "%s/state.dat", cdir);
    const int lr = sched_load(s, p, err, sizeof err);
    if (lr < 0 || pz_u256_bits(local) + s->cbits > 126) return;          /* unreadable state, or past the capped virtual space */
    const u128 v = ((u128)local->w[1] << 64 | local->w[0]) << s->cbits | off;
    u128 behind = 0;                                                        /* positions before v already scanned */
    bool scanned = false;
    for (size_t i = 0; i < s->ndone; i++) {
        if (v >= s->done[i].a && v < s->done[i].b) scanned = true;
        if (s->done[i].a < v) behind += (v < s->done[i].b ? v : s->done[i].b) - s->done[i].a;
    }
    if (scanned) { pz_prop_line("state", "already scanned"); return; }
    char rate[64] = "";
    double rt = 0;
    if (campaign_status_get(cdir, "rate_per_second", rate, sizeof rate)) rt = atof(rate);
    if (!(rt > 0)) { pz_prop_line("state", "not yet scanned"); return; }
    char t[48], r[32];
    pz_prop_line("state", "not yet scanned: reached in about %s at the last speed (%s %s/s)", pz_fmt_duration((double)(v - behind) / rt, t, sizeof t),
                 pz_fmt_si(rt, r, sizeof r), unit);
}

int cmd_where(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    if (!o.campaign[0] && o.puzzle) snprintf(o.campaign, sizeof o.campaign, "p%d", o.puzzle);
    if (!o.campaign[0]) pz_usage_error("where", "the campaign is missing: -c NAME or -p N");
    if (!o.key[0]) pz_usage_error("where", "the key is missing: --key HEX");
    campaign_cfg c;
    char p[700], err[256], cdir[600];
    snprintf(cdir, sizeof cdir, "%s/%s", o.dir, o.campaign);
    snprintf(p, sizeof p, "%s/campaign.conf", cdir);
    if (cfg_load(&c, p, err, sizeof err) != 0) pz_die("%s", err);
    pz_u256 given, key;
    if (!pz_u256_from_hex(&given, o.key)) pz_die("invalid key (hexadecimal digits expected)");
    if (c.mode == MODE_KANGAROO) pz_die("the campaign uses the kangaroo: it has no predetermined visit order, 'where' does not apply");
    char rng[3 * PZ_U256_HEX_BUF + 40];
    const char *ra = cfg_range_text(&c, rng, sizeof rng);       /* the order is of the index of the key (with --step, not the key) */
    if (!cfg_index_of(&c, &given, &key)) pz_die("the key is not among the keys of the campaign (%s)", ra);
    if (c.mode == MODE_BSGS) {
        char berr[200];
        bsgs_params bp;
        pz_ge q;
        if (!cfg_search_pubkey(&c, &q)) pz_die("the public key is not that of a key of the steps");
        if (!bsgs_params_init(&bp, &c.range_start, &c.range_end, &q, c.bsgs_t, berr, sizeof berr)) pz_die("%s", berr);
        u64 gi = bsgs_giant_index(&bp, &key);
        if (!gi) pz_die("the key is not in the range of the campaign (%s)", ra);
        campaign_cfg sc;
        bsgs_sched_cfg(&bp, &c, &sc);
        sched_t *s = sched_new(&sc);
        pz_u256 seq;
        u64 off;
        pz_u256 gk = pz_u256_from_u64(gi);
        if (!sched_locate(s, &gk, &seq, &off)) pz_die("giant index out of range");
        const long double nblocks = pz_u256_to_ld(&s->blocks_all);
        char step[32], steps[32], blk[80], blks[48], blk_g[112], blks_g[64];   /* one buffer for each text: the arguments are evaluated in any order */
        snprintf(blks, sizeof blks, "%.0Lf", nblocks);
        pz_prop_line("giant step", "the key will be found at step %s of %s", pz_fmt_u64(gi, step, sizeof step), pz_fmt_u64(bp.Ng, steps, sizeof steps));
        pz_prop_line("visit order", "block %s of %s, %.4Lf%% of the way", pz_group_digits(pz_u256_to_dec(&seq, blk), blk_g, sizeof blk_g), pz_group_digits(blks, blks_g, sizeof blks_g),
                     pz_u256_to_ld(&seq) * 100.0L / nblocks);
        where_state(s, cdir, &seq, off, "giant steps");
        sched_free(s);
        return 0;
    }
    sched_t *s = sched_new(&c);
    pz_u256 seq;
    u64 off;
    if (!sched_locate(s, &key, &seq, &off)) pz_die("the key is not in the range of the campaign (%s)", ra);
    char seq_text[80], block_grouped[64], blocks_grouped[64], blocks_plain[64], offset_text[32];
    const long double blocks = pz_u256_to_ld(&s->blocks_all);            /* of the whole search order, also with --part */
    snprintf(blocks_plain, sizeof blocks_plain, "%.0Lf", blocks);
    pz_prop_line("visit order", "block %s of %s, offset %s within the block, %.4Lf%% of the way", pz_group_digits(pz_u256_to_dec(&seq, seq_text), block_grouped, sizeof block_grouped),
                 pz_group_digits(blocks_plain, blocks_grouped, sizeof blocks_grouped), pz_fmt_u64(off, offset_text, sizeof offset_text), pz_u256_to_ld(&seq) * 100.0L / blocks);
    pz_u256 local = seq;
    bool mine = true;
    if (s->part.total) {
        mine = sched_part_position(s, &seq, &local);
        snprintf(blocks_plain, sizeof blocks_plain, "%.0Lf", pz_u256_to_ld(&s->blocks_part));
        if (mine)
            pz_prop_line("part", "in part %s of this computer: its block %s of %s", s->part_text, pz_group_digits(pz_u256_to_dec(&local, seq_text), block_grouped, sizeof block_grouped),
                         pz_group_digits(blocks_plain, blocks_grouped, sizeof blocks_grouped));
        else
            pz_prop_line("part", "not in part %s of this computer: another computer will scan it", s->part_text);
    }
    if (mine) where_state(s, cdir, &local, off, "keys");
    sched_free(s);
    return 0;
}

int cmd_split(int argc, char **argv)
{
    if (argc < 3) pz_usage_error("split", "the speeds of at least two computers are needed (keys per second: 800M, 1.5G)");
    const unsigned n = (unsigned)(argc - 1);
    if (n > 64) pz_die("at most 64 computers");
    u64 speed[64];
    unsigned counts[64];
    for (unsigned i = 0; i < n; i++) {
        double v;
        if (!pz_parse_rate(argv[i + 1], &v) || v < 1) pz_die("split: invalid speed '%s' (expected a speed such as 800M or 1.5G)", argv[i + 1]);
        speed[i] = (u64)(v + 0.5);
    }
    const unsigned total = 100;
    u128 wsum = 0;
    for (unsigned i = 0; i < n; i++) wsum += speed[i];
    if (!part_split(speed, n, total, counts)) pz_die("cannot divide %u shares among %u computers", total, n);
    printf("Shares proportional to the speeds (%u in total), one after the other:\n", total);
    unsigned first = 0;
    for (unsigned i = 0; i < n; i++) {
        part_spec ps;
        char txt[PART_TEXT], err[200];
        snprintf(txt, sizeof txt, "%u-%u/%u", first, first + counts[i] - 1, total);
        if (counts[i] == 1) snprintf(txt, sizeof txt, "%u/%u", first, total);
        if (!part_parse(txt, &ps, err, sizeof err)) pz_die("%s", err);
        part_format(&ps, txt, sizeof txt);
        printf("  computer %u:  --part %-10s %3u%% of the range%s\n", i + 1, txt, counts[i],
               (u128)speed[i] * 200 < (u128)wsum ? "   (too slow for this split: its minimum share is more than its fair share)" : "");
        first += counts[i];
    }
    pz_fputs_wrap(stdout, "On every computer use the same puzzle or range (-p or -r), the same -s and --chunk-bits, and -m linear.\n", PZ_LINE_MAX, 2);
    return 0;
}
