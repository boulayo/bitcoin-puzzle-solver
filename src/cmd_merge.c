/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd_merge.c - 'pzsolve merge': the distinguished points of other computers into a kangaroo campaign (kangaroo_merge), with the
 * encryption of the key and the banner of 'hunt' (cmd.h). */
#include "cmd.h"
#include "kangaroo.h"
#include "campaigns.h"
#include "onfound.h"

int cmd_merge(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    pz_log_set_quiet(o.quiet);
    if (!o.campaign[0] && o.puzzle) snprintf(o.campaign, sizeof o.campaign, "p%d", o.puzzle);
    if (!o.campaign[0]) pz_usage_error("merge", "the campaign is missing: -c NAME or -p N");
    if (!o.nfiles) pz_usage_error("merge", "the files to merge are missing: -f FILE_OR_FOLDER (the dp.bin of another computer, or its campaign folder)");
    char cdir[600], conf[700], err[256];
    snprintf(cdir, sizeof cdir, "%s/%s", o.dir, o.campaign);
    snprintf(conf, sizeof conf, "%s/campaign.conf", cdir);
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    if (cfg_load(&r->cfg, conf, err, sizeof err) != 0) pz_die("%s", err);
    if (r->cfg.mode != MODE_KANGAROO) pz_die("campaign '%s' does not use the kangaroo: 'merge' merges the distinguished points of the kangaroo", o.campaign);
    pz_strlcpy(r->cfg.dir, cdir, sizeof r->cfg.dir);
    run_paths(r);
    if (print_already_found(r)) {
        free(r);
        return 0;
    }
    check_encrypt_resume(&o, &r->cfg);
    campaign_lock(cdir);   /* a campaign that is running must be stopped first: both would write the same log */
    pz_strlcpy(r->on_found, o.on_found, sizeof r->on_found);
    if (o.encrypt_pubkey[0] || o.encrypt_password) harden_process();
    encrypt_on_resume(&o, r);
    announce_encrypt(r);
    r->mem_limit = mem_bytes(o.mem_gb);
    const char *fl[16];
    for (int i = 0; i < o.nfiles; i++) fl[i] = o.files[i];
    int rc = kangaroo_merge(r, fl, o.nfiles);
    if (atomic_load(&r->found)) {
        print_found_banner(r);
        onfound_wait();
        const int st = r->found_seal_failed ? PZ_EXIT_ERROR : 0;
        free(r);
        return st;
    }
    if (rc == 0) pz_fputs_wrap(stdout, "\nNo collision between the points yet: continue with 'hunt' on this campaign.\n", PZ_LINE_MAX, 2);
    else pz_fputs_wrap(stdout, "\nSome of the files could not be merged (see above).\n", PZ_LINE_MAX, 2);
    free(r);
    return rc ? PZ_EXIT_ERROR : 0;                  /* a file that could not be merged is an error, as for every command but hunt */
}
