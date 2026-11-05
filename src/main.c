/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* main.c - pzsolve command line interface. */
#include "run.h"
#include "cmd.h"
#include "selftest.h"
#include "doctor.h"
#include "kangaroo.h"
#include "bsgs.h"
#include "platform.h"
#include "envopts.h"
#include "encrypt.h"
#include "cmd_encrypt.h"
#include <getopt.h>
#include <errno.h>
#include <stdarg.h>

/* The legal notice that `pzsolve version` prints and that a version derived from this one has to keep (NOTICE, additional terms under
 * section 7 of the AGPL). */
static void print_legal_notice(FILE *f)
{
    fputs("Copyright (C) 2026 Leonardo Boulay\n"
          "License: GNU AGPL version 3 or later, with the additional terms of the NOTICE file (section 7 of the AGPL).\n"
          "This is free software: you may use, study, change and share it under those terms. There is NO WARRANTY.\n"
          "Source code and NOTICE: https://github.com/boulayo/bitcoin-puzzle-solver\n", f);
}

static void help_print(FILE *f, const char *text) { pz_fputs_wrap(f, text, PZ_LINE_MAX, -1); }

static const char *main_text(void)
{
    static char text[8192];
    snprintf(text, sizeof text,
"pzsolve %s - solver for the Bitcoin Puzzle (keys in [2^(n-1), 2^n-1])\n"
"\n"
"USAGE\n"
"  pzsolve <command> [options]\n"
"  pzsolve help <command>    the options of a command\n"
"\n"
"COMMANDS\n"
"  hunt       start (or resume) a search campaign\n"
"  info       the state of a campaign\n"
"  list       all campaigns of a folder: progress, speed, time left\n"
"  where      when a key will be visited (linear, BSGS)\n"
"  split      divide --part shares among computers by their speeds\n"
"  puzzle     the data of a puzzle and the effort to solve it\n"
"  puzzles    the 160 built-in puzzles\n"
"  devices    the CPU, SIMD instructions, RAM and GPUs\n"
"  selftest   the built-in correctness checks\n"
"  merge      add the distinguished points of other computers (kangaroo)\n"
"  encrypt-keygen   make the public key to which a campaign encrypts the key it finds (rented computers)\n"
"  decrypt-key      decrypt the key that a campaign found and encrypted\n"
"  doctor     check program, machine, GPUs, cache and campaigns\n"
"  bench      the speed of the CPU and GPU engines\n"
"  kstat      statistics of the kangaroo cost (cycle study)\n"
"  version    the version\n"
"\n"
"EXAMPLES\n"
"  pzsolve puzzles --open            the puzzles that are still unsolved\n"
"  pzsolve hunt -p 71                start or resume puzzle 71\n"
"  pzsolve hunt -p 71 -r 400000000000000000:5fffffffffffffffff   a part only\n"
"  pzsolve hunt -r 8000:ffff -a ADDRESS -c mine   a custom range and target\n"
"  pzsolve info -c p71\n"
"  pzsolve encrypt-keygen            then:  pzsolve hunt -p 71 --encrypt-pubkey PUBKEY   (the key is never shown in plain)\n"
"\n"
"Copyright (C) 2026 Leonardo Boulay. Free software under the GNU AGPL version 3 or later, with no warranty: 'pzsolve version'.\n", PZ_VERSION);
    return text;
}

static const char *hunt_text(void)
{
    static char text[16384];
    snprintf(text, sizeof text,
"pzsolve hunt - start (or resume) a search campaign\n"
"\n"
"USAGE\n"
"  pzsolve hunt (-p N | -r START:END) [-a ADDRESS | -k PUBKEY] [options]\n"
"\n"
"TARGET\n"
"  -p, --puzzle N          puzzle 1..256: range, address and (if known) public key are set automatically; the campaign is\n"
"                          named 'pN' and is resumed automatically if it already exists. Puzzles 161..256 were emptied by the\n"
"                          creator in 2017 (only their address is known). With -r it selects only the target\n"
"  -r, --range S:E         custom range in hexadecimal (inclusive); a key outside it is not reported, not even when the last block\n"
"                          of the linear search reaches it\n"
"  -a, --address ADDR      bitcoin address to find: \"1...\" or \"bc1q...\" (compressed key); required unless -k is used\n"
"  -k, --pubkey HEX        known public key (compressed 33 bytes or uncompressed 65 bytes): enables the kangaroo and BSGS\n"
"\n"
"CAMPAIGN\n"
"  -c, --campaign NAME     campaign name (folder <dir>/<name>); resumed if it exists [pN with -p N; otherwise a name derived\n"
"                          from the range and the target, so that the same command resumes it]\n"
"  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"
"  -s, --seed SEED         seed: 64 hex digits (used as they are) or any text (hashed); default or 'auto': generated at random\n"
"\n"
"METHOD\n"
"  -m, --mode M            auto | linear | kangaroo | bsgs   [auto: linear without a public key or for a range of less\n"
"                          than 2^20 keys; otherwise bsgs when a table that beats the kangaroo fits in RAM (or in --mem) and\n"
"                          one of these holds: no usable GPU (or --no-gpu), a range of a few seconds of work, --bsgs-entries\n"
"                          given, a --table file that exists; otherwise kangaroo]\n"
"\n"
"RESOURCES\n"
"  -t, --threads N         CPU threads [all available cores; without -t the linear search leaves one core to drive each\n"
"                          OpenCL GPU in use]\n"
"      --no-cpu            do not use the CPU\n"
"      --no-gpu            do not use the GPUs\n"
"      --limit X           use only X%% of the machine (1..99), always at full speed and without pauses: X%% of the CPU threads\n"
"                          (one per physical core first) and X%% of the GPU work-items; less heat and power, speed reduced\n"
"                          accordingly [off]. It does not use or update the machine tuning; with -t the X%% is computed on the\n"
"                          given threads\n"
"      --keep-awake        Windows and macOS: prevent the system from going to sleep while the campaign runs\n"
"\n"
"SEVERAL COMPUTERS (same -s, range and public key on all of them)\n"
"      --part Q/T          linear search: the blocks of the visit order are divided into T shares and this computer scans share Q\n"
"                          (counting from 0; several are allowed: 0,1/4 or 0-6/9) [without it: every block].\n"
"                          The same --chunk-bits on all the computers\n"
"      --node N            kangaroo: number of this computer (0..65535, 0 if there is only one): the kangaroos start from\n"
"                          different points (see 'merge')\n"
"\n"
"ENCRYPTING THE KEY (for rented computers; see 'pzsolve help encrypt-keygen' and 'pzsolve help decrypt-key')\n"
"      --encrypt-pubkey HEX\n"
"                          when the key is found, encrypt it to this public key (66 hex digits, or 130 uncompressed; made\n"
"                          by 'encrypt-keygen' on a computer you trust): the screen, FOUND.txt and the log then carry only\n"
"                          a line 'PZENC1:...', safe to copy anywhere, and neither the key nor its public key. Stored in\n"
"                          the campaign: a resumed campaign uses it, another one is refused. Core dumps are turned off.\n"
"                          Invalid: the program does not start (it never falls back to showing the key)\n"
"      --encrypt-password\n"
"                          instead of --encrypt-pubkey: type a password now (hidden, asked twice) and the public key is\n"
"                          made from it, as 'encrypt-keygen' does; the password is not stored. Only on a computer you trust\n"
"                          while you type: what sees the keyboard or the memory now can capture it. Not asked again when\n"
"                          the campaign already has its key\n"
"      --on-found COMMAND\n"
"                          run COMMAND once with the shell after FOUND.txt is written (at most 120 s; the search does not\n"
"                          wait for it), with PZ_FOUND_SEALED (the PZENC1 line, empty without encryption),\n"
"                          PZ_FOUND_ADDRESS, PZ_FOUND_PUZZLE, PZ_FOUND_CAMPAIGN and PZ_FOUND_FILE set, never the key. Not\n"
"                          stored: give it again at every start\n"
"\n"
"DISPLAY\n"
"      --ui U              auto | full (multi-line panel updated in place) | line (one line) | log (one line per minute, suited\n"
"                          to files)   [auto: full on a terminal]\n"
"  -q, --quiet             no progress display: only warnings, errors and the result\n"
"\n"
"ADVANCED (the defaults suit almost every campaign)\n"
"      --table FILE        bsgs: reads the baby-step table from FILE if it exists, otherwise builds it and saves it (the table\n"
"                          does not depend on the target: it is reused for any public key)\n"
"      --bsgs-entries N    bsgs: baby steps of the table when it is created [optimal for the range, within the RAM limits]\n"
"      --dp-bits B         kangaroo: bits of the distinguished points (0..60) [0: automatic]\n"
"      --mem GIB           kangaroo and bsgs: maximum RAM for the table, in GiB (1 GiB = 1073741824 bytes; 0.001 at least)\n"
"                          [kangaroo: 40%% of the free RAM, at least 256 MiB and at most 64 GiB; bsgs: the table is chosen to\n"
"                          fit in 70%% of the free RAM]\n"
"      --chunk-bits B      linear: size of the permuted blocks, 2^B keys (20..40) [as large as the range allows with 16 blocks\n"
"                          per share, at most 36]; smaller blocks slow down the CPU and above all the GPUs, which receive\n"
"                          one block per launch\n"
"      --gpu LIST          use only the listed GPUs (comma-separated indices, see 'devices'); it also allows OpenCL devices\n"
"                          that are CPUs\n"
"      --retune            measure again the best combination of CPU threads and GPUs for this machine (the first run of each\n"
"                          version measures it automatically, about 2 minutes, and remembers it); not used with -t, --gpu,\n"
"                          --no-cpu, --no-gpu or -m bsgs, nor without a GPU; refused with --limit\n"
"  -v, --verbose           more messages\n"
"  Diagnosis, through environment variables (USAGE.md section 7.10): PZ_ISA (the CPU engine), PZ_PRIO_GPU (the priority of the\n"
"  threads that serve the GPUs), PZ_GPU_API (CUDA or OpenCL), PZ_NO_PROFILE (no machine profile)\n"
"\n"
"EXIT STATUS\n"
"  0                       the key was found (also when the campaign had already found it: it is in FOUND.txt)\n"
"  1                       the search finished without finding the key (the whole range, or the whole --part, was covered)\n"
"  2                       error: invalid options, campaign in use, unreadable files, a state that could not be saved\n"
"  130                     interrupted by Ctrl-C, SIGTERM or the closing of the terminal: the state is saved, the same command resumes\n");
    return text;
}

static const char bench_text[] =
"pzsolve bench - measure the speed of the CPU and GPU engines\n"
"\n"
"USAGE\n"
"  pzsolve bench [--together [--linear] [-t N] [--gpu LIST] [--prio-gpu P]] [--gpu-only] [--variants [--turns N]] [--retune]\n"
"\n"
"Without options: every CPU engine, the kangaroo, BSGS, and the GPUs.\n"
"\n"
"OPTIONS\n"
"      --together          measure CPU and GPUs working together as in a campaign (puzzle 140), with several numbers of CPU\n"
"                          threads and with each slower GPU switched off in turn: reports the best configuration\n"
"      --linear            with --together: the linear search instead of the kangaroo\n"
"  -t, --threads N         with --together: try only N CPU threads\n"
"      --gpu LIST          with --together: use only the listed GPUs (as in hunt)\n"
"      --prio-gpu P        with --together: auto | yes | no (for hunt it is the environment variable PZ_PRIO_GPU)\n"
"      --gpu-only          skip the CPU tests and run only the GPU ones (bandwidth, integer instructions, linear search, kangaroo)\n"
"      --variants          GPUs only: every variant of the linear and kangaroo kernels of each GPU, measured in turns (in order,\n"
"                          reversed, in order), one table per kernel with the median, the spread and the tuner's choice, then the\n"
"                          length of a launch; nothing is saved unless --retune is given too\n"
"      --turns N           with --variants: N turns instead of 3 (1 to 9)\n"
"      --retune            ignore the saved choice of the GPU kernel variant and measure it again (with --variants: save what the\n"
"                          tables decide)\n";

/* The short help of the other commands: usage line, then one line per option. */
static const struct { const char *cmd, *text; } short_help[] = {
    {"info", "pzsolve info - show the state of an existing campaign (its report.txt)\n\nUSAGE\n  pzsolve info (-c NAME | -p N) [-d DIR]\n\nOPTIONS\n"
             "  -c, --campaign NAME     the campaign\n  -p, --puzzle N          the campaign of puzzle N (named pN)\n"
             "  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"},
    {"list", "pzsolve list - list all campaigns in the folder with progress, speed and time left\n\nUSAGE\n  pzsolve list [-d DIR]\n\nOPTIONS\n"
             "  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"},
    {"where", "pzsolve where - tell when a given key will be visited in the order of a linear or BSGS campaign\n\nUSAGE\n"
              "  pzsolve where (-c NAME | -p N) --key HEX [-d DIR]\n\nOPTIONS\n"
              "  -c, --campaign NAME     the campaign\n  -p, --puzzle N          the campaign of puzzle N (named pN)\n"
              "      --key HEX           the private key, in hexadecimal\n  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"},
    {"merge", "pzsolve merge - merge the distinguished points of other computers into a kangaroo campaign\n\nUSAGE\n"
              "  pzsolve merge (-c NAME | -p N) -f FILE_OR_FOLDER [-f ...] [-d DIR] [--mem GIB] [--encrypt-pubkey HEX | --encrypt-password] [--on-found COMMAND]\n\nOPTIONS\n"
              "  -c, --campaign NAME     the campaign (it must not be running)\n  -p, --puzzle N          the campaign of puzzle N (named pN)\n"
              "  -f, --file PATH         the dp.bin of another computer, or its campaign folder (up to 16)\n"
              "  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"
              "      --mem GIB           maximum RAM for the table of distinguished points, in GiB [as in hunt: 40% of the free RAM, at least\n"
              "                          256 MiB and at most 64 GiB]\n  -q, --quiet             fewer messages\n"
              "      --encrypt-pubkey HEX, --encrypt-password, --on-found COMMAND\n"
              "                          as in hunt (for a campaign that has no public key to encrypt to yet)\n"
              "\nEXIT STATUS\n  0                       the files were merged (or the key was found)\n"
              "  2                       a file could not be merged, or another error\n"},
    {"split", "pzsolve split - divide the --part shares among several computers in proportion to their speeds\n\nUSAGE\n"
              "  pzsolve split SPEED_1 SPEED_2 [...]\n\nARGUMENTS\n"
              "  SPEED                   keys per second of each computer, with k, M, G or T allowed (800M, 1.5G); at most 64\n"},
    {"doctor", "pzsolve doctor - check the program, machine, GPUs, cache and campaigns and report what is wrong\n\nUSAGE\n"
               "  pzsolve doctor [-d DIR] [--full]\n\nOPTIONS\n  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"
               "      --full              also run the full selftest, GPUs included\n"},
    {"puzzle", "pzsolve puzzle - show the data of a puzzle and the effort to solve it\n\nUSAGE\n  pzsolve puzzle N [--rate SPEED]\n\nOPTIONS\n"
               "      --rate SPEED        the speed used for the times, keys per second with k, M, G or T allowed (800M, 1.5G) [measured on\n"
               "                          the CPU of this machine]\n"},
    {"puzzles", "pzsolve puzzles - list the 160 built-in puzzles\n\nUSAGE\n  pzsolve puzzles [--open | --solved] [--pubkey]\n\nOPTIONS\n"
                "      --open              only the puzzles still unsolved\n      --solved            only the solved puzzles\n"
                "      --pubkey            only the puzzles whose public key is known\n"},
    {"devices", "pzsolve devices - list the detected CPU, SIMD instructions, RAM and GPUs\n\nUSAGE\n  pzsolve devices\n"},
    {"selftest", "pzsolve selftest - run the built-in correctness checks (CPU engines, kangaroo, BSGS, GPUs)\n\nUSAGE\n  pzsolve selftest\n"},
    {"kstat", "pzsolve kstat - statistics of the kangaroo cost over intervals of 2^BITS keys (cycle study)\n\nUSAGE\n"
              "  pzsolve kstat BITS TRIALS [THREADS [R [DP]]]\n\nARGUMENTS\n  BITS                    size of the intervals, 2^BITS keys (4..64)\n"
              "  TRIALS                  number of keys to find\n  THREADS                 CPU threads [1]\n"
              "  R, DP                   number of jumps and bits of the distinguished points (then the kangaroos without\n"
              "                          distinguished points are not restarted)\n"},
    {"encrypt-keygen", "pzsolve encrypt-keygen - make the public key to which the key found by a campaign is encrypted\n\nUSAGE\n  pzsolve encrypt-keygen [--random]\n\n"
                       "Run it on a computer you trust, not on the rented one. It asks for a password (hidden, twice), makes the secret key\n"
                       "from it (scrypt, about a second and 256 MiB) and prints the public key (66 hex digits) and its key id. Give the public\n"
                       "key to 'hunt --encrypt-pubkey'; keep the password: with it 'decrypt-key' recovers the key at home. Nothing is written\n"
                       "to disk. Without a terminal the password is one line of the input.\n"
                       "\n"
                       "The password is the only thing that protects the key: 7 or more random words or 20 or more random characters, never a\n"
                       "word or a name, because anyone who sees the public key can try passwords offline. A password lost is a key lost.\n"
                       "\nOPTIONS\n"
                       "      --random            no password: make a random private key and print it too (64 hex digits, with the public key).\n"
                       "                          Store it like a secret; 'decrypt-key --raw-key' reads it. Lost, the encrypted key is lost\n"},
    {"decrypt-key", "pzsolve decrypt-key - decrypt the key that a campaign found and encrypted\n\nUSAGE\n  pzsolve decrypt-key [--raw-key] [--address ADDRESS] [FILE | -]\n\n"
                       "Reads FILE (default: FOUND.txt in the current folder if there is one, otherwise standard input; - is standard input)\n"
                       "or a pasted text, finds every token 'PZENC1:...' in it and shows the key id it was encrypted to; the other lines are\n"
                       "ignored. Asks for the password (hidden), checks that it is the right one before it tries to decrypt, decrypts, and\n"
                       "prints the key (hex and WIF) and the addresses of the key (P2PKH and P2WPKH). A line opens with the secret alone. If\n"
                       "the text has an 'address:' line (FOUND.txt has one) or --address is given, it also checks that the key belongs to that\n"
                       "address and prints OK or MISMATCH; otherwise it says that the address was not checked. Exit status 0 when the key was\n"
                       "recovered (and, if there was an address, matches it); 1 when the secret does not open the line or the address does not\n"
                       "match; 2 when the input is not usable. Without a terminal the password is one line of standard input (the text then\n"
                       "comes from FILE).\n"
                       "\nOPTIONS\n"
                       "      --raw-key           read the private key (64 hex digits, hidden) instead of a password: for a key made with\n"
                       "                          'encrypt-keygen --random'\n"
                       "      --address ADDRESS   check the key against this address, when the text has no 'address:' line (or instead of it)\n"},
    {"version", "pzsolve version - print the version, the copyright and the license\n\nUSAGE\n  pzsolve version\n"},
};

/* The help text of a command (NULL: the general one); NULL when there is no such command. */
static const char *help_text(const char *topic)
{
    if (!topic) return main_text();
    if (!strcmp(topic, "hunt")) return hunt_text();
    if (!strcmp(topic, "bench")) return bench_text;
    for (size_t i = 0; i < sizeof short_help / sizeof *short_help; i++)
        if (!strcmp(topic, short_help[i].cmd)) return short_help[i].text;
    return NULL;
}

/* The help of a command: "pzsolve help hunt", "pzsolve hunt --help". False when there is no such command. */
bool pz_help(const char *topic, FILE *f)
{
    const char *t = help_text(topic);
    if (t) help_print(f, t);
    return t != NULL;
}

/* A command line that cannot be run: the message, then the usage line of the command, taken from its help (the only copy), and where to
 * read the rest. */
void pz_usage_error(const char *cmd, const char *fmt, ...)
{
    char msg[600], use[400] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const char *t = cmd && cmd[0] ? help_text(cmd) : NULL, *u = t ? strstr(t, "USAGE\n") : NULL;
    if (u) {
        u += 6;
        while (*u == ' ') u++;
        snprintf(use, sizeof use, "\n  usage: %.*s", (int)strcspn(u, "\n"), u);
    }
    if (t) pz_die("%s%s\n  (see 'pzsolve help %s')", msg, use, cmd);
    pz_die("%s (see 'pzsolve help')", msg);
}

static const char *g_cmd = "";

enum { O_TABLE = 998, O_BSGSN = 997, O_UI = 999, O_NOCPU = 1000, O_NOGPU, O_GPU, O_CHUNK, O_DP, O_MEM, O_KEY, O_RETUNE, O_AWAKE, O_NODE, O_PART, O_LIMIT, O_ENCPUB, O_ENCPW, O_ONFOUND };

static const struct option long_opts[] = {
    {"puzzle", 1, 0, 'p'}, {"range", 1, 0, 'r'}, {"address", 1, 0, 'a'}, {"pubkey", 1, 0, 'k'},
    {"campaign", 1, 0, 'c'}, {"dir", 1, 0, 'd'}, {"seed", 1, 0, 's'}, {"mode", 1, 0, 'm'}, {"threads", 1, 0, 't'},
    {"no-cpu", 0, 0, O_NOCPU}, {"no-gpu", 0, 0, O_NOGPU}, {"gpu", 1, 0, O_GPU},
    {"chunk-bits", 1, 0, O_CHUNK}, {"dp-bits", 1, 0, O_DP}, {"mem", 1, 0, O_MEM}, {"quiet", 0, 0, 'q'}, {"verbose", 0, 0, 'v'},
    {"key", 1, 0, O_KEY}, {"ui", 1, 0, O_UI}, {"table", 1, 0, O_TABLE}, {"bsgs-entries", 1, 0, O_BSGSN},
    {"retune", 0, 0, O_RETUNE}, {"keep-awake", 0, 0, O_AWAKE}, {"node", 1, 0, O_NODE},
    {"part", 1, 0, O_PART}, {"limit", 1, 0, O_LIMIT}, {"encrypt-pubkey", 1, 0, O_ENCPUB}, {"encrypt-password", 0, 0, O_ENCPW},
    {"on-found", 1, 0, O_ONFOUND}, {"file", 1, 0, 'f'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};

/* The options removed in 1.23.0 and what takes their place: each one is refused with its own message, not as an unknown option. The
 * names are those of the command line, between blanks; dropping an entry makes that option an unknown one. */
static const struct { const char *names, *msg; } removed_opts[] = {
    {" -H --hash160 ", "--hash160 (-H) was removed in 1.23.0: give the address with -a"},
    {" --new --resume ", "--new and --resume were removed in 1.23.0: hunt creates the campaign or resumes it"},
    {" --gpu-api ", "--gpu-api was removed in 1.23.0: set the environment variable PZ_GPU_API=cuda|opencl instead"},
    {" --no-tune ", "--no-tune was removed in 1.23.0: set the environment variable PZ_NO_PROFILE=1 instead"},
    {" --report-interval ", "--report-interval was removed in 1.23.0: the panel is updated every second"},
    {" --checkpoint-interval ", "--checkpoint-interval was removed in 1.23.0: the state is saved every 60 seconds"},
    {" --isa ", "--isa was removed in 1.23.0: set the environment variable PZ_ISA instead (same values: auto, baseline, avx2, ...)"},
    {" --prio-gpu ", "--prio-gpu was removed in 1.23.0: set the environment variable PZ_PRIO_GPU=auto|yes|no instead"},
};

/* An option that getopt_long did not recognize ("-H", "--isa", "--isa=avx2"): the message of removed_opts when it is one of those. */
static void check_removed_option(const char *arg)
{
    char name[48];
    const size_t l = strcspn(arg, "=");
    if (l + 3 > sizeof name) return;
    snprintf(name, sizeof name, " %.*s ", (int)l, arg);
    for (size_t i = 0; i < sizeof removed_opts / sizeof *removed_opts; i++)
        if (strstr(removed_opts[i].names, name)) pz_die("%s", removed_opts[i].msg);
}

/* ---- the values of the options: the whole text must be a number in its range (atoi would take "12abc"); integers go through
 * pz_arg_int (util.c) ---- */

/* --mem: GiB, at least 1 MiB (less leaves no room for a table: 32 bytes would make a table of no entries) and at most 1e9 GiB, which still
 * fits in 64 bits of bytes. */
static double opt_mem_gib(const char *opt, const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno == ERANGE || !(v >= 0.001) || v > 1e9)
        pz_die("%s: invalid value '%s' (expected GiB, at least 0.001 and at most 1000000000)", opt, s);
    return v;
}

/* Copies the value of an option into a field of the options: a value that does not fit is refused, not cut. */
static void opt_copy(const char *opt, char *dst, size_t n, const char *val)
{
    if (strlen(val) >= n) pz_die("%s: the value is too long (at most %zu characters)", opt, n - 1);
    memcpy(dst, val, strlen(val) + 1);
}

static u64 opt_u64(const char *opt, const char *s, u64 lo, u64 hi)
{
    u64 v;
    if (!pz_parse_u64(s, &v) || v < lo || v > hi) pz_die("%s: invalid value '%s' (expected an integer between %llu and %llu)", opt, s, (unsigned long long)lo, (unsigned long long)hi);
    return v;
}

/* A device name of Windows, with or without an extension ("con", "NUL.txt", "com1", "lpt9"): no file or folder can have it there. */
static bool windows_device_name(const char *n)
{
    static const char *const dev[] = {"con", "prn", "aux", "nul"};
    char head[8];
    size_t l = strcspn(n, ".");
    if (l == 0 || l >= sizeof head) return false;
    for (size_t i = 0; i < l; i++) head[i] = (char)((n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i]);
    head[l] = 0;
    for (size_t i = 0; i < sizeof dev / sizeof *dev; i++)
        if (!strcmp(head, dev[i])) return true;
    return l == 4 && (!strncmp(head, "com", 3) || !strncmp(head, "lpt", 3)) && head[3] >= '0' && head[3] <= '9';
}

/* Campaign names become directory names: keep them to a safe alphabet (no path separators, no leading dot), and to names that every
 * system can have, because a campaign folder is copied between computers (Windows refuses a trailing dot and the device names). */
static bool valid_campaign_name(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 80 || n[0] == '.' || n[l - 1] == '.' || windows_device_name(n)) return false;
    for (size_t i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

/* The text of the option getopt_long has just rejected (unknown, or missing its value). A short option, alone or in a cluster ("-xq"), is
 * named by optopt: the element of argv that getopt is working on is not the one before optind then. A long one has been consumed whole. */
static const char *bad_option(char **argv, char *buf, size_t n)
{
    if (optopt > 32 && optopt < 127) {
        snprintf(buf, n, "-%c", optopt);
        return buf;
    }
    return argv[optind - 1];
}

/* The options each command takes (the long names, between blanks); hunt takes all of them but those named in `not`. Any other is refused, rather
 * than silently ignored. */
static const struct { const char *cmd, *only, *not; } cmd_opts[] = {
    {"hunt", NULL, " key file "},
    {"info", " campaign puzzle dir ", NULL},
    {"list", " dir ", NULL},
    {"where", " campaign puzzle dir key ", NULL},
    {"merge", " campaign puzzle dir file mem quiet encrypt-pubkey encrypt-password on-found ", NULL},
};

/* Refuses the option `c` (as getopt_long returned it) when the command does not use it. */
static void check_option_used(int c)
{
    const struct option *lo = NULL;
    for (const struct option *q = long_opts; q->name; q++)
        if (q->val == c) { lo = q; break; }
    if (!lo || c == 'h') return;
    char key[40];
    snprintf(key, sizeof key, " %s ", lo->name);
    for (size_t i = 0; i < sizeof cmd_opts / sizeof *cmd_opts; i++) {
        if (strcmp(cmd_opts[i].cmd, g_cmd) != 0) continue;
        const bool used = cmd_opts[i].only ? strstr(cmd_opts[i].only, key) != NULL : !strstr(cmd_opts[i].not, key);
        if (used) return;
        char sh[8] = "";
        if (c > 32 && c < 127) snprintf(sh, sizeof sh, " (-%c)", c);
        pz_usage_error(g_cmd, "the option --%s%s is not used by '%s'", lo->name, sh, g_cmd);
    }
}

void parse_opts(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof *o);
    o->threads = -1;
    o->chunk_bits = -1;
    o->dp_bits = -1;
    o->mem_gb = -1;
    snprintf(o->dir, sizeof o->dir, "campaigns");
    snprintf(o->mode, sizeof o->mode, "auto");
    optind = 1;
    int c;
    char optbuf[8];
    opterr = 0;
    while ((c = getopt_long(argc, argv, ":p:r:a:k:c:d:s:m:t:f:qvh", long_opts, NULL)) != -1) {
        check_option_used(c);
        switch (c) {
        case 'p': o->puzzle = (int)pz_arg_int("--puzzle", optarg, 1, 256); break;
        case 'r': opt_copy("--range", o->range, sizeof o->range, optarg); break;
        case 'a': opt_copy("--address", o->address, sizeof o->address, optarg); break;
        case 'k': opt_copy("--pubkey", o->pubkey, sizeof o->pubkey, optarg); break;
        case 'c':
            if (!valid_campaign_name(optarg)) pz_die("invalid campaign name '%s' (allowed: letters, digits, '-', '_', '.'; at most 80 characters; cannot start or end with '.' or be a Windows device name such as con, nul, com1 or lpt1)", optarg);
            pz_strlcpy(o->campaign, optarg, sizeof o->campaign);
            break;
        case 'd':
            if (!optarg[0]) pz_die("--dir: invalid value '' (expected the name of a folder)");
            opt_copy("--dir", o->dir, sizeof o->dir, optarg);
            break;
        case 's': opt_copy("--seed", o->seed, sizeof o->seed, optarg); break;
        case 'm': opt_copy("--mode", o->mode, sizeof o->mode, optarg); break;
        case 't': o->threads = (int)pz_arg_int("--threads", optarg, 1, RUN_MAX_THREADS); break;
        case O_NOCPU: o->no_cpu = true; break;
        case O_NOGPU: o->no_gpu = true; break;
        case O_RETUNE: o->retune = true; break;
        case O_AWAKE: o->keep_awake = true; break;
        case O_PART: {
            part_spec ps;
            char perr[300];
            if (!part_parse(optarg, &ps, perr, sizeof perr)) pz_die("--part: %s", perr);
            part_format(&ps, o->part, sizeof o->part);                 /* canonical; empty when it means the whole order */
            if (!o->part[0]) pz_strlcpy(o->part, "0/1", sizeof o->part);
            break;
        }
        case O_LIMIT: o->limit = (int)pz_arg_int("--limit", optarg, 1, 99); break;
        case O_NODE: o->node = (int)pz_arg_int("--node", optarg, 0, 65535); o->node_given = true; break;
        case 'f':
            if (o->nfiles >= 16) pz_die("at most 16 files at a time with -f");
            opt_copy("--file", o->files[o->nfiles], sizeof o->files[0], optarg);
            o->nfiles++;
            break;
        case O_GPU:
            if (!pz_index_list_valid(optarg)) pz_die("--gpu: invalid list '%s' (comma-separated indices, see 'devices')", optarg);
            opt_copy("--gpu", o->gpu, sizeof o->gpu, optarg);
            break;
        case O_CHUNK: o->chunk_bits = (int)pz_arg_int("--chunk-bits", optarg, 20, 40); break;   /* smaller blocks starve the GPUs and slow the CPU */
        case O_DP: o->dp_bits = (int)pz_arg_int("--dp-bits", optarg, 0, 60); break;
        case O_MEM: o->mem_gb = opt_mem_gib("--mem", optarg); break;
        case 'q': o->quiet = true; break;
        case 'v': o->verbose = true; break;
        case O_ENCPUB: opt_copy("--encrypt-pubkey", o->encrypt_pubkey, sizeof o->encrypt_pubkey, optarg); break;
        case O_ENCPW: o->encrypt_password = true; break;
        case O_ONFOUND: opt_copy("--on-found", o->on_found, sizeof o->on_found, optarg); break;
        case O_KEY: opt_copy("--key", o->key, sizeof o->key, optarg); break;
        case O_TABLE: opt_copy("--table", o->table, sizeof o->table, optarg); break;
        case O_BSGSN: o->bsgs_entries = opt_u64("--bsgs-entries", optarg, 1, BS_MAX_T); break;
        case O_UI:
            if (!strcmp(optarg, "auto")) o->ui = UI_AUTO;
            else if (!strcmp(optarg, "full")) o->ui = UI_FULL;
            else if (!strcmp(optarg, "line")) o->ui = UI_LINE;
            else if (!strcmp(optarg, "log")) o->ui = UI_LOG;
            else pz_die("--ui: invalid value '%s' (expected auto, full, line or log)", optarg);
            break;
        case 'h':
            if (!pz_help(g_cmd, stdout)) pz_help(NULL, stdout);
            exit(0);
        case ':': pz_usage_error(g_cmd, "the option '%s' needs a value", bad_option(argv, optbuf, sizeof optbuf));
        default:
            check_removed_option(bad_option(argv, optbuf, sizeof optbuf));
            pz_usage_error(g_cmd, "unrecognized option '%s'", bad_option(argv, optbuf, sizeof optbuf));
        }
    }
    if (optind < argc) {
        /* a bare value is almost always a forgotten option name ("hunt 71" instead of "hunt -p 71") */
        const char *a = argv[optind];
        const bool is_number = a[0] >= '0' && a[0] <= '9';
        if (is_number) pz_usage_error(g_cmd, "unexpected argument '%s' (did you mean -p %s?)", a, a);
        pz_usage_error(g_cmd, "unexpected argument '%s' (options start with - or --)", a);
    }
    if (o->encrypt_pubkey[0] && o->encrypt_password) pz_die("--encrypt-pubkey and --encrypt-password cannot be used together: give the public key, or type a password");
    if (o->encrypt_pubkey[0]) {                   /* before anything is created: a key that does not parse is a refusal, never a run that shows the key */
        pz_ge q;
        u8 q33[33];
        char err[240];
        if (!enc_parse_pubkey(o->encrypt_pubkey, &q, q33, err, sizeof err)) pz_die("--encrypt-pubkey: %s (nothing was started)", err);
    }
}

int main(int argc, char **argv)
{
    pz_console_init();
    pz_env_init();                                         /* the only place that reads the environment (envopts.h) */
    if (argc < 2) {
        pz_help(NULL, stderr);
        if (pz_console_is_own()) {
            /* double-clicked in Explorer: the window would vanish before the text could be read */
            fprintf(stderr, "\nThis is a command-line program: open PowerShell (or the Command Prompt) in its folder\n"
                            "and run for example  .\\pzsolve.exe puzzles --open\n\nPress Enter to close...");
            getchar();
        }
        return PZ_EXIT_ERROR;
    }
    const char *cmd = argv[1];
    g_cmd = cmd;
    if (argc == 3 && (!strcmp(argv[2], "--help") || !strcmp(argv[2], "-h")) && pz_help(cmd, stdout)) return 0;   /* every command: "pzsolve puzzle --help" */
    const bool noargs = !strcmp(cmd, "devices") || !strcmp(cmd, "selftest") || !strcmp(cmd, "version");
    if (noargs && argc > 2) pz_usage_error(cmd, "the command '%s' takes no arguments ('%s' not recognized)", cmd, argv[2]);
    if (!strcmp(cmd, "hunt")) return cmd_hunt(argc - 1, argv + 1);
    if (!strcmp(cmd, "info")) return cmd_info(argc - 1, argv + 1);
    if (!strcmp(cmd, "list")) return cmd_list(argc - 1, argv + 1);
    if (!strcmp(cmd, "where")) return cmd_where(argc - 1, argv + 1);
    if (!strcmp(cmd, "split")) return cmd_split(argc - 1, argv + 1);
    if (!strcmp(cmd, "puzzle")) return cmd_puzzle(argc, argv);
    if (!strcmp(cmd, "puzzles")) return cmd_puzzles(argc, argv);
    if (!strcmp(cmd, "devices")) return cmd_devices();
    if (!strcmp(cmd, "selftest")) return selftest_run(argc - 1, argv + 1);
    if (!strcmp(cmd, "bench")) return bench_run(argc - 1, argv + 1);
    if (!strcmp(cmd, "doctor")) return doctor_run(argc - 1, argv + 1);
    if (!strcmp(cmd, "merge")) return cmd_merge(argc - 1, argv + 1);
    if (!strcmp(cmd, "kstat")) return kangaroo_stats(argc, argv);
    if (!strcmp(cmd, "encrypt-keygen")) return encrypt_keygen_main(argc - 1, argv + 1);
    if (!strcmp(cmd, "decrypt-key")) return decrypt_key_main(argc - 1, argv + 1);
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        printf("pzsolve %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        print_legal_notice(stdout);
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        if (argc > 3) pz_die("help takes one command at most ('%s' not recognized)", argv[3]);
        if (!pz_help(argc > 2 ? argv[2] : NULL, stdout)) pz_die("no help for '%s': not a command (see 'pzsolve help')", argv[2]);
        return 0;
    }
    pz_die("unknown command '%s' (see 'pzsolve help')", cmd);
    return PZ_EXIT_ERROR;
}
