/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* cmd.h - the commands of the command line, one file each (cmd_hunt.c, cmd_merge.c, cmd_campaigns.c, cmd_puzzle.c, cmd_devices.c; the
 * others have headers of their own: cmd_encrypt.h, doctor.h, selftest.h, kangaroo.h), and what they share: the options that hunt, info,
 * list, where and merge take, read in main.c, and the helpers of hunt that merge uses too. main.c holds main, the dispatch and the help
 * texts. A command is called with argv[0] its own name (hunt, info, list, where, split, merge) or the name of the program (puzzle, puzzles). */
#ifndef PZ_CMD_H
#define PZ_CMD_H

#include "run.h"

#define PLURAL(n) ((n) == 1 ? "" : "s")

/* the CPU engines that PZ_ISA accepts on this architecture (the others are not compiled in) */
#if defined(__aarch64__)
#define PZ_ISA_LIST "auto, baseline, neon, neon2"
#else
#define PZ_ISA_LIST "auto, baseline, avx2, shani, avx512, avx2v, avx512v, avx512i"
#endif

/* ---- the options of hunt, info, list, where and merge ---- */

typedef struct {
    int puzzle;
    char range[200], address[80], pubkey[160], campaign[96], dir[400], seed[200], mode[16], gpu[128];
    int threads, chunk_bits, dp_bits;
    double mem_gb;
    bool no_cpu, no_gpu, quiet, verbose, retune, keep_awake;
    char part[PART_TEXT];
    int node, nfiles;
    bool node_given;              /* --node was given, 0 included */
    int limit;                    /* --limit: percentage of the machine to use (1..99; 0 = no limit) */
    char files[16][600];
    char key[100];
    char encrypt_pubkey[200];     /* --encrypt-pubkey */
    bool encrypt_password;        /* --encrypt-password */
    char on_found[1024];          /* --on-found */
    char jump_seed[80];           /* --jump-seed: the public seed of the jumps of a shared walk (docs/en/NETWORK.md 6) */
    char start_key[600];          /* --start-key: the file with its secret start key */
    char table[600];
    u64 bsgs_entries;
    ui_mode ui;
} opts_t;

/* Reads the options of the command (argv[0] is its name) into o, with their defaults; a message and the exit on an option the command does
 * not take, a value out of its range or a bare argument (main.c). */
void parse_opts(int argc, char **argv, opts_t *o);
/* The bytes of a --mem value; 0 when it was not given. */
static inline u64 mem_bytes(double gib) { return gib > 0 ? (u64)(gib * 1073741824.0) : 0; }

/* ---- the commands ---- */

int cmd_hunt(int argc, char **argv);
int cmd_merge(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_where(int argc, char **argv);
int cmd_split(int argc, char **argv);
int cmd_puzzle(int argc, char **argv);
int cmd_puzzles(int argc, char **argv);
int cmd_devices(void);

/* ---- what hunt and merge share (cmd_hunt.c): the encryption of the key that is found, and the key when it has been ---- */

/* Core dumps and same-user ptrace off, once. */
void harden_process(void);
/* The campaign c already encrypts to a public key: --encrypt-pubkey must name the same one (a message and the exit otherwise). */
void check_encrypt_resume(const opts_t *o, const campaign_cfg *c);
/* A campaign that is resumed or merged, with its lock taken: one that has no public key yet gets the one of the options. */
void encrypt_on_resume(const opts_t *o, run_ctx *r);
/* What a campaign that encrypts the key says at the start of a session. */
void announce_encrypt(const run_ctx *r);
/* The banner printed when the key is found. */
void print_found_banner(const run_ctx *r);
/* A campaign that has already found the key: FOUND.txt is shown. False when it has not. */
bool print_already_found(const run_ctx *r);

#endif
