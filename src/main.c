/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* main.c - pzsolve command line interface. */
#include "run.h"
#include "gpu.h"
#include "hash.h"
#include "selftest.h"
#include "profile.h"
#include "doctor.h"
#include "kangaroo.h"
#include "puzzles.h"
#include "campaigns.h"
#include "bsgs.h"
#include "platform.h"
#include "outfmt.h"
#include <getopt.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define PLURAL(n) ((n) == 1 ? "" : "s")

/* instruction sets that --isa accepts on this architecture (the others are not compiled in) */
#if defined(__aarch64__)
#define PZ_ISA_LIST "auto, baseline, neon, neon2"
#define PZ_ISA_HELP "auto | baseline | neon | neon2\n                          [auto: chosen with a micro-benchmark; neon and neon2 use the hardware SHA-256 of the CPU\n                          with 4 and 8 keys at a time; without it the baseline is used]"
#else
#define PZ_ISA_LIST "auto, baseline, avx2, avx512, avx2v, avx512v, avx512i"
#define PZ_ISA_HELP "auto | baseline | avx2 | avx512 | avx2v | avx512v | avx512i\n                          [auto: chosen with a micro-benchmark; 'v' = curve arithmetic in\n                          parallel on the SIMD lanes; 'i' = like avx512v but with AVX-512 IFMA]"
#endif

static void help_print(FILE *f, const char *text) { pz_fputs_wrap(f, text, PZ_LINE_MAX, -1); }

static void usage_main(FILE *f)
{
    static char text[8192];
    snprintf(text, sizeof text,
"pzsolve %s - solver for the Bitcoin Puzzle (keys in [2^(n-1), 2^n-1])\n"
"\n"
"USAGE\n"
"  pzsolve <command> [options]\n"
"  pzsolve help <command>    the options of a command (hunt, bench)\n"
"\n"
"COMMANDS\n"
"  hunt       start (or resume) a search campaign\n"
"  info       show the state of an existing campaign\n"
"  list       list all campaigns in the folder with progress, speed and time left\n"
"  where      tell when a given key will be visited in the campaign order: (-c NAME | -p N) --key HEX\n"
"  split      divide the --part shares among several computers in proportion to their speeds: split 800M 120M ...\n"
"  puzzle N   show data for puzzle N (address, public key, status) and effort estimates [--rate KEYS_PER_SECOND, e.g. 800M or 1.5G]\n"
"  puzzles    list the 160 built-in puzzles [--open | --solved | --pubkey]\n"
"  devices    list the detected CPU, SIMD instructions, RAM and GPUs\n"
"  selftest   run the built-in correctness checks\n"
"  merge      merge the distinguished points of other computers into a kangaroo campaign: -c NAME (or -p N) -f FILE_OR_FOLDER ... [-d DIR]\n"
"  doctor     check the program, machine, GPUs, cache and campaigns and report what is wrong [-d DIR] [--full]\n"
"  bench      measure the speed of the CPU and GPU engines\n"
"  kstat      BITS TRIALS [THREADS [R [DP]]]: statistics of the kangaroo cost over intervals of 2^BITS keys (cycle study)\n"
"  version    print the version\n"
"\n"
"EXAMPLES\n"
"  pzsolve puzzles --open            list the puzzles that are still unsolved\n"
"  pzsolve hunt -p 71                start (or resume) puzzle 71: everything is already in the database\n"
"  pzsolve hunt -p 71 -r 400000000000000000:5fffffffffffffffff   only part of the range\n"
"  pzsolve hunt -p 66 -a 13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so -c test   custom target\n"
"  pzsolve info -c p71\n", PZ_VERSION);
    help_print(f, text);
}

static void usage_hunt(FILE *f)
{
    static char text[16384];
    snprintf(text, sizeof text,
"pzsolve hunt - start (or resume) a search campaign\n"
"\n"
"USAGE\n"
"  pzsolve hunt (-p N | -r START:END) [-a ADDRESS | -H HASH160 | -k PUBKEY] [options]\n"
"\n"
"TARGET\n"
"  -p, --puzzle N          puzzle 1..256: range, address and (if known) public key are set automatically; the campaign is\n"
"                          named 'pN' and is resumed automatically if it already exists. Puzzles 161..256 were emptied by the\n"
"                          creator in 2017\n"
"  -r, --range S:E         custom range in hexadecimal (inclusive)\n"
"  -a, --address ADDR      bitcoin address to find: \"1...\" or \"bc1q...\" (compressed key); required unless -H or -k is used\n"
"  -H, --hash160 HEX       instead of the address: its hash160 (40 hex digits)\n"
"  -k, --pubkey HEX        known public key (compressed 33 bytes or uncompressed 65 bytes): enables the kangaroo and BSGS\n"
"\n"
"CAMPAIGN\n"
"  -c, --campaign NAME     campaign name (folder <dir>/<name>); resumed if it exists\n"
"  -d, --dir DIR           base folder of the campaigns [./campaigns]\n"
"  -s, --seed SEED         seed: 64 hex digits or any text; default: generated at random\n"
"      --new               fail if the campaign already exists\n"
"      --resume            fail if the campaign does not exist\n"
"\n"
"METHOD\n"
"  -m, --mode M            auto | linear | kangaroo | bsgs   [auto: with a known public key, bsgs if the table fits in RAM\n"
"                          (and there are no GPUs or the range is small), otherwise kangaroo; without it, linear]\n"
"      --table FILE        bsgs: reads the baby-step table from FILE if it exists, otherwise builds it and saves it (the table\n"
"                          does not depend on the target: it is reused for any public key)\n"
"      --bsgs-entries N    bsgs: baby steps of the table when it is created [optimal for the range, within the RAM limits]\n"
"      --dp-bits B         kangaroo: bits of the distinguished points (0 = automatic)\n"
"      --mem GB            kangaroo: maximum RAM for the table of distinguished points [auto]\n"
"      --chunk-bits B      linear: size of the permuted blocks, 2^B keys (20..40) [36 for large ranges]\n"
"\n"
"RESOURCES\n"
"  -t, --threads N         CPU threads [all available cores]\n"
"      --no-cpu            do not use the CPU\n"
"      --no-gpu            do not use the GPUs\n"
"      --gpu LIST          use only the listed GPUs (comma-separated indices, see 'devices')\n"
"      --gpu-api A         auto | cuda | opencl   [auto: CUDA for NVIDIA cards (OpenCL as a fallback), OpenCL for the others]\n"
"      --isa I             " PZ_ISA_HELP "\n"
"      --limit X           use only X%% of the machine (1..99), always at full speed and without pauses: X%% of the CPU threads\n"
"                          (one per physical core first) and X%% of the GPU work-items; less heat and power, speed reduced\n"
"                          accordingly [off]. It does not use or update the machine tuning; with -t the X%% is computed on the\n"
"                          given threads\n"
"      --prio-gpu P        auto | yes | no: the threads that serve the GPUs (they launch the kernels) run at a higher priority than\n"
"                          the compute threads [auto: the tuning tests it and remembers it; Linux requires root]\n"
"      --retune            measure again the best combination of CPU threads and GPUs for this machine (the first run of each\n"
"                          version measures it automatically, about 2 minutes, and remembers it); not used with -t, --gpu,\n"
"                          --no-cpu or --no-gpu\n"
"      --no-tune           do not measure or use the machine profile: all threads and all GPUs\n"
"      --keep-awake        Windows and macOS: prevent the system from going to sleep while the campaign runs\n"
"\n"
"SEVERAL COMPUTERS (same -s, range and public key on all of them)\n"
"      --part Q/T          linear search: the blocks of the visit order are divided into T shares and this computer scans share Q\n"
"                          (counting from 0; several are allowed: 0,1/4 or 0-6/9). Same --chunk-bits on all\n"
"      --node N            kangaroo: number of this computer (0..65535, 0 if there is only one): the kangaroos start from\n"
"                          different points (see 'merge')\n"
"\n"
"DISPLAY AND FILES\n"
"      --ui U              auto | full (multi-line panel updated in place) | line (one line) | log (one line per minute, suited\n"
"                          to files)   [auto: full on a terminal]\n"
"      --no-progress       same as --ui log\n"
"      --report-interval S seconds between panel updates [1]\n"
"      --checkpoint-interval S   seconds between state saves [60]\n"
"  -q, --quiet             fewer messages\n"
"  -v, --verbose           more messages\n");
    help_print(f, text);
}

static void usage_bench(FILE *f)
{
    help_print(f,
"pzsolve bench - measure the speed of the CPU and GPU engines\n"
"\n"
"USAGE\n"
"  pzsolve bench [--together [--linear] [-t N] [--gpu LIST] [--prio-gpu P]] [--gpu-only] [--retune]\n"
"\n"
"Without options: every CPU engine, the kangaroo, BSGS, and the GPUs.\n"
"\n"
"OPTIONS\n"
"      --together          measure CPU and GPUs working together as in a campaign (puzzle 140), with several numbers of CPU\n"
"                          threads and with each slower GPU switched off in turn: reports the best configuration\n"
"      --linear            with --together: the linear search instead of the kangaroo\n"
"  -t, --threads N         with --together: try only N CPU threads\n"
"      --gpu LIST          with --together: use only the listed GPUs (as in hunt)\n"
"      --prio-gpu P        with --together: auto | yes | no, as in hunt\n"
"      --gpu-only          skip the CPU tests and run only the GPU ones (bandwidth, integer instructions, linear search, kangaroo)\n"
"      --retune            ignore the saved choice of the GPU kernel variant and measure it again\n");
}

/* The help of a command: "pzsolve help hunt", "pzsolve hunt --help". */
void pz_help(const char *topic, FILE *f)
{
    if (topic && !strcmp(topic, "hunt")) usage_hunt(f);
    else if (topic && !strcmp(topic, "bench")) usage_bench(f);
    else usage_main(f);
}

static const char *g_cmd = "";

typedef struct {
    int puzzle;
    char range[200], address[80], hash160[48], pubkey[160], campaign[96], dir[400], seed[200], mode[16], isa[16], gpu[128];
    int threads, chunk_bits, dp_bits;
    double mem_gb;
    int report_interval, checkpoint_interval;
    bool no_cpu, no_gpu, no_progress, quiet, verbose, want_new, want_resume, retune, no_tune, keep_awake;
    prio_mode prio_gpu;
    char part[PART_TEXT];
    int node, nfiles;
    int limit;                    /* --limit: percentage of the machine to use (1..100; 100 = no limit) */
    char files[16][600];
    char key[100];
    char table[600];
    u64 bsgs_entries;
    ui_mode ui;
} opts_t;

enum { O_GPUAPI = 996, O_TABLE = 998, O_BSGSN = 997, O_UI = 999, O_NOCPU = 1000, O_NOGPU, O_GPU, O_ISA, O_CHUNK, O_DP, O_MEM, O_REPORT, O_CKPT, O_NOPROG, O_NEW, O_RESUME, O_KEY, O_RETUNE, O_NOTUNE, O_AWAKE, O_NODE, O_PRIO, O_PART, O_LIMIT };

static const struct option long_opts[] = {
    {"puzzle", 1, 0, 'p'}, {"range", 1, 0, 'r'}, {"address", 1, 0, 'a'}, {"hash160", 1, 0, 'H'}, {"pubkey", 1, 0, 'k'},
    {"campaign", 1, 0, 'c'}, {"dir", 1, 0, 'd'}, {"seed", 1, 0, 's'}, {"mode", 1, 0, 'm'}, {"threads", 1, 0, 't'},
    {"no-cpu", 0, 0, O_NOCPU}, {"no-gpu", 0, 0, O_NOGPU}, {"gpu", 1, 0, O_GPU}, {"isa", 1, 0, O_ISA},
    {"chunk-bits", 1, 0, O_CHUNK}, {"dp-bits", 1, 0, O_DP}, {"mem", 1, 0, O_MEM}, {"report-interval", 1, 0, O_REPORT},
    {"checkpoint-interval", 1, 0, O_CKPT}, {"no-progress", 0, 0, O_NOPROG}, {"quiet", 0, 0, 'q'}, {"verbose", 0, 0, 'v'},
    {"new", 0, 0, O_NEW}, {"resume", 0, 0, O_RESUME}, {"key", 1, 0, O_KEY}, {"ui", 1, 0, O_UI}, {"table", 1, 0, O_TABLE}, {"bsgs-entries", 1, 0, O_BSGSN}, {"gpu-api", 1, 0, O_GPUAPI},
    {"retune", 0, 0, O_RETUNE}, {"no-tune", 0, 0, O_NOTUNE}, {"keep-awake", 0, 0, O_AWAKE}, {"node", 1, 0, O_NODE}, {"prio-gpu", 1, 0, O_PRIO},
    {"part", 1, 0, O_PART}, {"limit", 1, 0, O_LIMIT}, {"file", 1, 0, 'f'}, {"help", 0, 0, 'h'},
    /* former Italian spellings: still accepted, not documented */
    {"ritara", 0, 0, O_RETUNE}, {"no-taratura", 0, 0, O_NOTUNE}, {"sveglio", 0, 0, O_AWAKE}, {"nodo", 1, 0, O_NODE}, {"parte", 1, 0, O_PART},
    {"limite", 1, 0, O_LIMIT}, {0, 0, 0, 0}};

/* Strict numeric option parsing: the whole string must be a number in range (atoi() would silently accept "12abc").
 * pz_parse_* rather than strtoll: on glibc >= 2.38 strtoll resolves to __isoc23_strtoll and the binary would refuse
 * to start on older distributions. */
static long long opt_int(const char *opt, const char *s, long long lo, long long hi)
{
    i64 v;
    if (!pz_parse_i64(s, &v) || v < lo || v > hi) pz_die("%s: invalid value '%s' (expected an integer between %lld and %lld)", opt, s, lo, hi);
    return v;
}

static double opt_positive_double(const char *opt, const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno == ERANGE || !(v > 0) || v > 1e12) pz_die("%s: invalid value '%s' (expected a positive number)", opt, s);
    return v;
}

/* A speed such as 800000000, 800M or 1.5G (suffixes k, M, G, T). */
static double opt_rate(const char *opt, const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end != s && end[0] && !end[1]) {
        switch (end[0]) {
        case 'k': case 'K': v *= 1e3; end++; break;
        case 'm': case 'M': v *= 1e6; end++; break;
        case 'g': case 'G': v *= 1e9; end++; break;
        case 't': case 'T': v *= 1e12; end++; break;
        default: break;
        }
    }
    if (end == s || *end || errno == ERANGE || !(v > 0) || v > 1e15) pz_die("%s: invalid value '%s' (expected a speed such as 800M or 1.5G)", opt, s);
    return v;
}

static u64 opt_u64(const char *opt, const char *s, u64 lo, u64 hi)
{
    u64 v;
    if (!pz_parse_u64(s, &v) || v < lo || v > hi) pz_die("%s: invalid value '%s' (expected an integer between %llu and %llu)", opt, s, (unsigned long long)lo, (unsigned long long)hi);
    return v;
}

/* Campaign names become directory names: keep them to a safe alphabet (no path separators, no leading dot). */
static bool valid_campaign_name(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 80 || n[0] == '.') return false;
    for (size_t i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

static void parse_opts(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof *o);
    o->threads = -1;
    o->chunk_bits = -1;
    o->dp_bits = -1;
    o->mem_gb = -1;
    o->report_interval = -1;
    o->checkpoint_interval = -1;
    snprintf(o->dir, sizeof o->dir, "campaigns");
    snprintf(o->mode, sizeof o->mode, "auto");
    snprintf(o->isa, sizeof o->isa, "auto");
    optind = 1;
    int c;
    opterr = 0;
    while ((c = getopt_long(argc, argv, ":p:r:a:H:k:c:d:s:m:t:f:qvh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'p': o->puzzle = (int)opt_int("--puzzle", optarg, 1, 256); break;
        case 'r': pz_strlcpy(o->range, optarg, sizeof o->range); break;
        case 'a': pz_strlcpy(o->address, optarg, sizeof o->address); break;
        case 'H': pz_strlcpy(o->hash160, optarg, sizeof o->hash160); break;
        case 'k': pz_strlcpy(o->pubkey, optarg, sizeof o->pubkey); break;
        case 'c':
            if (!valid_campaign_name(optarg)) pz_die("invalid campaign name '%s' (allowed: letters, digits, '-', '_', '.'; at most 80 characters; cannot start with '.')", optarg);
            pz_strlcpy(o->campaign, optarg, sizeof o->campaign);
            break;
        case 'd': pz_strlcpy(o->dir, optarg, sizeof o->dir); break;
        case 's': pz_strlcpy(o->seed, optarg, sizeof o->seed); break;
        case 'm': pz_strlcpy(o->mode, optarg, sizeof o->mode); break;
        case 't': o->threads = (int)opt_int("--threads", optarg, 1, MAX_WORKERS - 16); break;
        case O_NOCPU: o->no_cpu = true; break;
        case O_NOGPU: o->no_gpu = true; break;
        case O_RETUNE: o->retune = true; break;
        case O_NOTUNE: o->no_tune = true; break;
        case O_AWAKE: o->keep_awake = true; break;
        case O_PRIO:
            if (!strcmp(optarg, "auto")) o->prio_gpu = PRIO_AUTO;
            else if (!strcmp(optarg, "yes") || !strcmp(optarg, "si")) o->prio_gpu = PRIO_YES;   /* "si": former spelling */
            else if (!strcmp(optarg, "no")) o->prio_gpu = PRIO_NO;
            else pz_die("--prio-gpu must be auto, yes or no");
            break;
        case O_PART: {
            part_spec ps;
            char perr[300];
            if (!part_parse(optarg, &ps, perr, sizeof perr)) pz_die("--part: %s", perr);
            part_format(&ps, o->part, sizeof o->part);                 /* canonical; empty when it means the whole order */
            if (!o->part[0]) pz_strlcpy(o->part, "0/1", sizeof o->part);
            break;
        }
        case O_LIMIT: o->limit = (int)opt_int("--limit", optarg, 1, 100); break;
        case O_NODE: o->node = (int)opt_int("--node", optarg, 0, 65535); break;
        case 'f':
            if (o->nfiles >= 16) pz_die("at most 16 files at a time with -f");
            pz_strlcpy(o->files[o->nfiles++], optarg, sizeof o->files[0]);
            break;
        case O_GPU:
            for (const char *q = optarg;; q++) {   /* digits separated by single commas: "0", "0,2" */
                bool digit = *q >= '0' && *q <= '9';
                bool sep_ok = (*q == ',' || *q == 0) && q > optarg && q[-1] >= '0' && q[-1] <= '9';
                if (!digit && !sep_ok) pz_die("--gpu: invalid list '%s' (comma-separated indices, see 'devices')", optarg);
                if (!*q) break;
            }
            pz_strlcpy(o->gpu, optarg, sizeof o->gpu);
            break;
        case O_GPUAPI:
            if (!strcmp(optarg, "auto")) gpu_set_api_pref(GPU_API_AUTO);
            else if (!strcmp(optarg, "cuda")) gpu_set_api_pref(GPU_API_CUDA);
            else if (!strcmp(optarg, "opencl")) gpu_set_api_pref(GPU_API_OPENCL);
            else pz_die("--gpu-api must be auto, cuda or opencl");
            break;
        case O_ISA: pz_strlcpy(o->isa, optarg, sizeof o->isa); break;
        case O_CHUNK: o->chunk_bits = (int)opt_int("--chunk-bits", optarg, 20, 40); break;   /* smaller blocks starve the GPUs and slow the CPU */
        case O_DP: o->dp_bits = (int)opt_int("--dp-bits", optarg, 0, 60); break;
        case O_MEM: o->mem_gb = opt_positive_double("--mem", optarg); break;
        case O_REPORT: o->report_interval = (int)opt_int("--report-interval", optarg, 1, 86400); break;
        case O_CKPT: o->checkpoint_interval = (int)opt_int("--checkpoint-interval", optarg, 1, 86400); break;
        case O_NOPROG: o->no_progress = true; break;
        case 'q': o->quiet = true; break;
        case 'v': o->verbose = true; break;
        case O_NEW: o->want_new = true; break;
        case O_RESUME: o->want_resume = true; break;
        case O_KEY: pz_strlcpy(o->key, optarg, sizeof o->key); break;
        case O_TABLE: pz_strlcpy(o->table, optarg, sizeof o->table); break;
        case O_BSGSN: o->bsgs_entries = opt_u64("--bsgs-entries", optarg, 1, BS_MAX_T); break;
        case O_UI:
            if (!strcmp(optarg, "auto")) o->ui = UI_AUTO;
            else if (!strcmp(optarg, "full")) o->ui = UI_FULL;
            else if (!strcmp(optarg, "line")) o->ui = UI_LINE;
            else if (!strcmp(optarg, "log")) o->ui = UI_LOG;
            else pz_die("--ui must be auto, full, line or log");
            break;
        case 'h': pz_help(g_cmd, stdout); exit(0);
        case ':': pz_die("the option '%s' needs a value (see 'pzsolve help %s')", argv[optind - 1], g_cmd);
        default: pz_die("unrecognized option '%s' (see 'pzsolve help %s')", argv[optind - 1], g_cmd);
        }
    }
    if (optind < argc) {
        /* a bare value is almost always a forgotten option name ("hunt 71" instead of "hunt -p 71") */
        const char *a = argv[optind];
        bool num = a[0] >= '0' && a[0] <= '9';
        if (num) pz_die("unexpected argument '%s' (did you mean -p %s?)", a, a);
        pz_die("unexpected argument '%s' (options start with - or --: see 'pzsolve help %s')", a, g_cmd);
    }
    if (o->want_new && o->want_resume) pz_die("--new and --resume are mutually exclusive: --new requires a new campaign, --resume an existing one");
}

static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* --isa is validated before a campaign directory is created, so a typo leaves nothing behind. */
static void check_isa_option(const char *isa)
{
    if (!strcmp(isa, "auto")) return;
    const cpu_engine *e = cpu_engine_by_name(isa);
    if (!e) pz_die("unknown --isa: %s (valid: %s)", isa, PZ_ISA_LIST);
    if (!e->supported()) pz_die("the CPU does not support the instruction set '%s' (see 'pzsolve devices')", isa);
}

static bool parse_pubkey(const char *hex, campaign_cfg *c)
{
    u8 b[65];
    int n = pz_hex_decode(hex, b, 65);
    if (n != 33 && n != 65) return false;
    ge p;
    if (!ge_from_pubkey(&p, b, (size_t)n)) return false;
    c->pubkey = p;
    ge_to_pub33(c->pubkey33, &p);
    c->has_pubkey = true;
    return true;
}

/* Table size for a bsgs campaign: forced with --bsgs-entries, taken from an existing --table file, or the best one that
 * fits the memory budget (70% of the available RAM, or --mem). *viable (optional) tells whether it beats the kangaroo. */
static u64 pick_bsgs_entries(const opts_t *o, const u256 *rsz, bool *viable)
{
    sysinfo_t si;
    sysinfo_detect(&si);
    u64 budget = o->mem_gb > 0 ? (u64)(o->mem_gb * 1073741824.0) : (u64)(0.70 * (double)si.mem_avail);
    u64 T;
    char err[256];
    if (o->table[0] && access(o->table, F_OK) == 0) {
        if (!bsgs_table_peek(o->table, &T, err, sizeof err)) pz_die("table %s is not usable: %s", o->table, err);
        if (viable) *viable = bsgs_mean_cost(rsz, T) <= KANG_C_MEAN * sqrtl(u256_to_ld(rsz));
        return T;
    }
    if (o->bsgs_entries) {
        T = o->bsgs_entries;
        if (viable) *viable = bsgs_mean_cost(rsz, T) <= KANG_C_MEAN * sqrtl(u256_to_ld(rsz));
        return T;
    }
    T = bsgs_choose_T(rsz, budget, viable);
    if (T < 1) pz_die("not enough memory for a BSGS table");
    return T;
}

/* Name used when -c is absent. It depends only on what identifies the search (puzzle, range, target), so repeating the same
 * command resumes the campaign instead of starting a new one. */
static void derive_campaign_name(const opts_t *o, char *out, size_t n)
{
    if (o->puzzle && !o->range[0]) {
        snprintf(out, n, "p%d", o->puzzle);
        return;
    }
    char rng[300] = "", key[900];
    if (o->range[0]) {
        char tmp[200];
        pz_strlcpy(tmp, o->range, sizeof tmp);
        char *colon = strchr(tmp, ':');
        u256 a, b;
        if (colon && (*colon = 0, u256_from_hex(&a, tmp) && u256_from_hex(&b, colon + 1))) {
            char ha[80], hb[80];
            u256_to_hex(&a, ha);
            u256_to_hex(&b, hb);
            snprintf(rng, sizeof rng, "%s:%s", ha, hb);   /* normalised: leading zeros / case do not matter */
        } else pz_strlcpy(rng, o->range, sizeof rng);
    }
    int kl = snprintf(key, sizeof key, "PZNAME1|%d|%s|%s|%s|%s", o->puzzle, rng, o->address, o->hash160, o->pubkey);
    u8 h[32];
    char id[17];
    sha256((const u8 *)key, (size_t)(kl < (int)sizeof key ? kl : (int)sizeof key - 1), h);
    pz_hex_encode(h, 4, id);
    if (o->puzzle) snprintf(out, n, "p%d-%s", o->puzzle, id);
    else snprintf(out, n, "custom-%s", id);
}

/* Builds a new campaign configuration from the command line. */
static void build_new_cfg(const opts_t *o, campaign_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->format_version = PZ_FORMAT_VERSION;
    pz_strlcpy(c->created_with, PZ_VERSION, sizeof c->created_with);
    c->created_at = pz_now();
    if (o->range[0]) {
        char tmp[200];
        pz_strlcpy(tmp, o->range, sizeof tmp);
        char *colon = strchr(tmp, ':');
        if (!colon) pz_die("--range requires the format START:END (hexadecimal)");
        *colon = 0;
        if (!u256_from_hex(&c->range_start, tmp) || !u256_from_hex(&c->range_end, colon + 1)) pz_die("--range: invalid hexadecimal values");
        c->puzzle = o->puzzle;
    } else if (o->puzzle) {
        if (!puzzle_range(o->puzzle, &c->range_start, &c->range_end)) pz_die("puzzle %d is outside the supported limits (1..256)", o->puzzle);
        c->puzzle = o->puzzle;
    } else {
        pz_die("specify the range with --puzzle N or --range START:END");
    }
    if (u256_is_zero(&c->range_start)) pz_die("the range must start from a key >= 1");
    if (u256_cmp(&c->range_start, &c->range_end) > 0) pz_die("empty range (start > end)");
    if (u256_cmp(&c->range_end, &SECP_N) >= 0) pz_die("the range exceeds the order of the secp256k1 group");

    bool have_target = false;
    const puzzle_info *db = o->puzzle ? puzzle_find(o->puzzle) : NULL;
    bool from_db = false;
    if (db && !o->address[0] && !o->hash160[0] && !o->pubkey[0]) {
        /* target taken from the built-in database */
        u8 h[20];
        if (!address_to_hash160(db->address, h)) pz_die("puzzle database corrupted (puzzle %d)", db->n);
        memcpy(c->target_h160, h, 20);
        pz_strlcpy(c->address, db->address, sizeof c->address);
        have_target = true;
        from_db = true;
        if (db->pubkey[0] && parse_pubkey(db->pubkey, c)) {
            u8 hp[20];
            hash160(c->pubkey33, 33, hp);
            if (memcmp(hp, h, 20) != 0) pz_die("puzzle database inconsistent (puzzle %d)", db->n);
        }
        if (db->privkey[0]) {
            /* a solved puzzle: the stored key must map to the stored address, otherwise the database is damaged */
            u256 pk;
            ge pt;
            u8 pub[33], hk[20];
            if (!u256_from_hex(&pk, db->privkey) || !ec_mul_g(&pt, &pk)) pz_die("puzzle database corrupted (key of puzzle %d)", db->n);
            ge_to_pub33(pub, &pt);
            hash160(pub, 33, hk);
            if (memcmp(hk, h, 20) != 0) pz_die("puzzle database inconsistent (the key of puzzle %d does not match the address)", db->n);
            pz_log(PZ_LOG_WARN, "puzzle %d is already SOLVED (the key is public): the campaign is only a test run", db->n);
            if (o->range[0] && (u256_cmp(&pk, &c->range_start) < 0 || u256_cmp(&pk, &c->range_end) > 0))
                pz_log(PZ_LOG_WARN, "the known key of puzzle %d is not in the given range: the campaign will not find it", db->n);
        }
        if (o->range[0]) {
            /* a custom range must overlap the range the puzzle guarantees */
            u256 pa, pb;
            if (puzzle_range(o->puzzle, &pa, &pb)) {
                if (u256_cmp(&c->range_end, &pa) < 0 || u256_cmp(&c->range_start, &pb) > 0)
                    pz_die("the given range does not intersect the one of puzzle %d: the key cannot be there", o->puzzle);
                if (u256_cmp(&c->range_start, &pa) < 0 || u256_cmp(&c->range_end, &pb) > 0)
                    pz_log(PZ_LOG_WARN, "the given range extends beyond the one of puzzle %d: the outside part cannot contain the key", o->puzzle);
            }
        }
    }
    if (o->pubkey[0]) {
        if (!parse_pubkey(o->pubkey, c)) pz_die("invalid public key (expected 33 or 65 bytes in hex, a point on the curve)");
        hash160(c->pubkey33, 33, c->target_h160);
        have_target = true;
    }
    if (o->address[0]) {
        u8 h[20];
        if (!address_to_hash160(o->address, h)) pz_die("invalid address (a mainnet \"1...\" address or a native SegWit \"bc1q...\" address with a valid checksum is expected)");
        if (have_target && memcmp(h, c->target_h160, 20) != 0)
            pz_die("the given public key does not match the address (the hash160 of the compressed form differs)");
        memcpy(c->target_h160, h, 20);
        pz_strlcpy(c->address, o->address, sizeof c->address);
        have_target = true;
    }
    if (o->hash160[0]) {
        u8 h[20];
        if (pz_hex_decode(o->hash160, h, 20) != 20) pz_die("--hash160 requires 40 hexadecimal digits");
        if (have_target && memcmp(h, c->target_h160, 20) != 0) pz_die("--hash160 does not match the given address/pubkey");
        memcpy(c->target_h160, h, 20);
        have_target = true;
    }
    if (db && !from_db) {
        u8 hd[20];
        if (address_to_hash160(db->address, hd) && memcmp(hd, c->target_h160, 20) != 0)
            pz_log(PZ_LOG_WARN, "the given target is not the one of puzzle %d in the database (%s): using the given one", db->n, db->address);
    }
    const char *retired = o->puzzle ? puzzle_retired_address(o->puzzle) : NULL;
    if (!have_target && retired) {
        /* puzzles 161..256: the address is known, the funds are gone */
        u8 h[20];
        if (!address_to_hash160(retired, h)) pz_die("puzzle database corrupted (puzzle %d)", o->puzzle);
        memcpy(c->target_h160, h, 20);
        pz_strlcpy(c->address, retired, sizeof c->address);
        have_target = true;
        pz_log(PZ_LOG_WARN, "puzzle %d was emptied by the creator in 2017 (block 475240): address %s holds no "
               "funds, the search is only an exercise (and with 2^%d keys it cannot finish)", o->puzzle, retired, o->puzzle - 1);
    }
    if (!have_target) pz_die("a target is required: --address, --hash160 or --pubkey");
    if (!c->address[0]) hash160_to_address(c->target_h160, c->address, sizeof c->address);

    if (o->seed[0] && strcmp(o->seed, "auto") != 0) seed_from_text(o->seed, c->seed);
    else seed_generate(c->seed);

    u256 rsz = cfg_range_size(c);
    c->chunk_bits = o->chunk_bits > 0 ? (unsigned)o->chunk_bits : cfg_default_chunk_bits(&rsz);
    u256 chunk = u256_pow2(c->chunk_bits);
    if (u256_cmp(&chunk, &rsz) > 0) {   /* chunk larger than the range: shrink */
        unsigned b = u256_bits(&rsz) - 1;
        c->chunk_bits = b < 1 ? 1 : b;
    }
    c->dp_bits = o->dp_bits > 0 ? (unsigned)o->dp_bits : 0;

    if (!strcmp(o->mode, "linear")) c->mode = MODE_LINEAR;
    else if (!strcmp(o->mode, "kangaroo")) {
        if (!c->has_pubkey) pz_die("kangaroo mode requires --pubkey");
        c->mode = MODE_KANGAROO;
    } else if (!strcmp(o->mode, "bsgs")) {
        if (!c->has_pubkey) pz_die("bsgs mode requires the public key");
        c->mode = MODE_BSGS;
        c->bsgs_t = pick_bsgs_entries(o, &rsz, NULL);
    } else if (!strcmp(o->mode, "auto")) {
        u256 lim = u256_pow2(20);
        if (!c->has_pubkey || u256_cmp(&rsz, &lim) < 0) c->mode = MODE_LINEAR;
        else {
            bool viable;
            u64 T = pick_bsgs_entries(o, &rsz, &viable);
            bool given = o->table[0] && access(o->table, F_OK) == 0;
            bool small = viable && bsgs_mean_cost(&rsz, T) < 4e9L;               /* seconds of work: no point in waking the GPUs */
            bool gpus = !o->no_gpu && gpu_count_usable() > 0;
            if (viable && (given || small || !gpus || o->bsgs_entries)) {
                c->mode = MODE_BSGS;
                c->bsgs_t = T;
            } else c->mode = MODE_KANGAROO;
        }
    } else pz_die("--mode must be auto, linear, kangaroo or bsgs");

    if (o->part[0] && strcmp(o->part, "0/1") != 0) {
        if (c->mode != MODE_LINEAR) pz_die("--part is only used in linear search (with a known public key use -m linear; the kangaroo has --node and 'merge')");
        pz_strlcpy(c->part, o->part, sizeof c->part);
    }
    if (c->mode == MODE_LINEAR) {                 /* before anything is created on disk: the order of visit must exist and every share must hold a block */
        char perr[300];
        if (!sched_check_part(c, perr, sizeof perr)) pz_die("%s", perr);
    }

    pz_strlcpy(c->name, o->campaign, sizeof c->name);   /* always set by cmd_hunt (explicit or derived) */
}

/* Options that describe the campaign itself must agree with the stored configuration: a silent mismatch would make the
 * user believe a different search is running than the one that resumes. */
static void check_resume_consistency(const opts_t *o, const campaign_cfg *c)
{
    if (o->puzzle && c->puzzle != o->puzzle) {
        if (c->puzzle) pz_die("the existing campaign is for puzzle %d, not %d", c->puzzle, o->puzzle);
        pz_die("the existing campaign is not associated with a puzzle of the database (use -c %s without -p)", c->name);
    }
    if (o->range[0]) {
        char tmp[200];
        pz_strlcpy(tmp, o->range, sizeof tmp);
        char *colon = strchr(tmp, ':');
        u256 a, b;
        if (!colon) pz_die("--range requires the format START:END (hexadecimal)");
        *colon = 0;
        if (!u256_from_hex(&a, tmp) || !u256_from_hex(&b, colon + 1)) pz_die("--range: invalid hexadecimal values");
        if (u256_cmp(&a, &c->range_start) != 0 || u256_cmp(&b, &c->range_end) != 0)
            pz_die("--range does not match the range of the existing campaign (the range cannot be changed; use another name with -c)");
    }
    if (o->address[0]) {
        u8 h[20];
        if (!address_to_hash160(o->address, h) || memcmp(h, c->target_h160, 20) != 0) pz_die("the given address does not match the one of the existing campaign");
    }
    if (o->hash160[0]) {
        u8 h[20];
        if (pz_hex_decode(o->hash160, h, 20) != 20 || memcmp(h, c->target_h160, 20) != 0) pz_die("--hash160 does not match the one of the existing campaign");
    }
    if (o->pubkey[0]) {
        campaign_cfg tmp;
        memset(&tmp, 0, sizeof tmp);
        if (!parse_pubkey(o->pubkey, &tmp)) pz_die("invalid public key (expected 33 or 65 bytes in hex, a point on the curve)");
        if (!c->has_pubkey || memcmp(tmp.pubkey33, c->pubkey33, 33) != 0) pz_die("the given public key does not match the one of the existing campaign");
    }
    if (o->seed[0] && strcmp(o->seed, "auto") != 0) {
        u8 s[32];
        seed_from_text(o->seed, s);
        if (memcmp(s, c->seed, 32) != 0) pz_die("the given seed does not match the one of the existing campaign (the seed cannot be changed)");
    }
    if (o->part[0]) {
        const char *want = strcmp(o->part, "0/1") ? o->part : "";
        if (strcmp(want, c->part) != 0)
            pz_die("--part %s does not match the split of the existing campaign (%s): the split cannot be changed, use another name with -c",
                   o->part, c->part[0] ? c->part : "none");
    }
    if (o->chunk_bits > 0 && (unsigned)o->chunk_bits != c->chunk_bits) pz_die("--chunk-bits cannot be changed on an existing campaign (it is %u)", c->chunk_bits);
    if (strcmp(o->mode, "auto") != 0) {
        const char *want = !strcmp(o->mode, "linear") ? "linear" : !strcmp(o->mode, "kangaroo") ? "kangaroo" : !strcmp(o->mode, "bsgs") ? "bsgs" : NULL;
        if (!want) pz_die("--mode must be auto, linear, kangaroo or bsgs");
        if (strcmp(want, mode_name(c->mode)) != 0)
            pz_die("the existing campaign uses mode %s, not %s (the mode cannot be changed: use another name with -c)", mode_name(c->mode), want);
    }
    if (o->bsgs_entries && c->mode == MODE_BSGS && o->bsgs_entries != c->bsgs_t)
        pz_die("--bsgs-entries cannot be changed on an existing campaign (the table has %llu entries)", (unsigned long long)c->bsgs_t);
}

static void parse_gpu_filter(run_ctx *r, const char *list) { pz_strlcpy(r->gpu_filter, list, sizeof r->gpu_filter); }

/* The size of a range in words: "8,388,608 keys", "2^43 keys", "about 6.65e35 keys". */
static void size_text(const u256 *sz, const char *dec, char *out, size_t n)
{
    const size_t len = strlen(dec);
    int bits = 0, top = -1;
    for (int i = 0; i < 256; i++)
        if ((sz->w[i / 64] >> (i % 64)) & 1) { bits++; top = i; }
    if (len <= 9 || (len <= 12 && bits != 1)) {
        char g[48];
        snprintf(out, n, "%s keys", pz_group_digits(dec, g, sizeof g));
    } else if (bits == 1) snprintf(out, n, "2^%d keys", top);
    else snprintf(out, n, "about %c.%c%ce%zu keys", dec[0], dec[1], dec[2], len - 1);
}

/* The banner printed when the key is found (by 'hunt' or by 'merge'). */
static void print_found_banner(const run_ctx *r, const char *addr, const char *hx, const char *wif)
{
    printf("\a\n======================= KEY FOUND =======================\n");
    if (r->cfg.puzzle) printf("  puzzle:         #%d\n", r->cfg.puzzle);
    printf("  address:        %s\n  key (hex):      %s\n  WIF compressed: %s\n", addr, hx, wif);
    printf("  saved in:       %s\n=========================================================\n", r->path_found);
}

static int cmd_hunt(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    pz_log_set_quiet(o.quiet);
    pz_log_set_verbose(o.verbose);
    check_isa_option(o.isa);
    if (o.no_cpu && o.no_gpu) pz_die("--no-cpu and --no-gpu together: no compute device selected");
    if (o.gpu[0] && !o.no_gpu) gpu_check_filter(o.gpu);   /* before anything is created on disk */
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    char cdir[600], conf[700];
    if (!o.campaign[0]) derive_campaign_name(&o, o.campaign, sizeof o.campaign);   /* stable name: re-running resumes */
    snprintf(cdir, sizeof cdir, "%s/%s", o.dir, o.campaign);
    snprintf(conf, sizeof conf, "%s/campaign.conf", cdir);
    bool resuming = file_exists(conf);
    if (o.want_new && resuming) pz_die("campaign '%s' already exists (use another name, or drop --new to resume it)", o.campaign);
    if (o.want_resume && !resuming) pz_die("--resume: campaign '%s' does not exist", o.campaign);
    if (resuming) {
        char err[256];
        if (cfg_load(&r->cfg, conf, err, sizeof err) != 0) pz_die("%s", err);
        check_resume_consistency(&o, &r->cfg);
        pz_strlcpy(r->cfg.dir, cdir, sizeof r->cfg.dir);
    } else {
        build_new_cfg(&o, &r->cfg);
        snprintf(r->cfg.dir, sizeof r->cfg.dir, "%s/%s", o.dir, r->cfg.name);
        if (pz_mkdir_p(r->cfg.dir) != 0) pz_die("cannot create folder %s: %s", r->cfg.dir, strerror(errno));
        snprintf(conf, sizeof conf, "%s/campaign.conf", r->cfg.dir);
        if (file_exists(conf)) pz_die("campaign '%s' already exists", r->cfg.name);
        if (cfg_save(&r->cfg, conf) != 0) pz_die("cannot save %s", conf);
    }
    run_paths(r);
    if (resuming && file_exists(r->path_found)) {
        char *t = pz_read_file(r->path_found, NULL);
        printf("This campaign has already found the key (%s):\n\n%s\n", r->path_found, t ? t : "");
        free(t);
        free(r);
        return 0;
    }
    campaign_lock(r->cfg.dir);   /* released at process exit */
    pz_log_set_file(r->path_log);

    r->nthreads = o.threads > 0 ? o.threads : r->sys.ncpu;
    if (r->nthreads > MAX_WORKERS - 16) {
        pz_log(PZ_LOG_WARN, "%d cores detected: using at most %d threads", r->nthreads, MAX_WORKERS - 16);
        r->nthreads = MAX_WORKERS - 16;
    }
    r->threads_explicit = o.threads > 0;
    if (o.node && r->cfg.mode != MODE_KANGAROO) pz_die("--node is only used with the kangaroo (in linear search split the range with -r)");
    r->node = (u32)o.node;
    r->use_cpu = !o.no_cpu;
    r->use_gpu = !o.no_gpu;
    if (o.gpu[0]) parse_gpu_filter(r, o.gpu);
    if (o.report_interval > 0) r->report_interval = o.report_interval;
    if (o.checkpoint_interval > 0) r->checkpoint_interval = o.checkpoint_interval;
    r->ui = (o.no_progress || o.quiet) ? UI_LOG : o.ui;
    if (o.mem_gb > 0) r->mem_limit = (u64)(o.mem_gb * 1073741824.0);
    pz_strlcpy(r->table_path, o.table, sizeof r->table_path);
    if (o.dp_bits >= 0) r->cfg.dp_bits = (unsigned)o.dp_bits;

    if (r->use_cpu && r->cfg.mode == MODE_LINEAR) {
        if (strcmp(o.isa, "auto") == 0) r->engine = cpu_engine_autotune(0.25, o.verbose);
        else {
            r->engine = cpu_engine_by_name(o.isa);             /* known and supported: check_isa_option() has already said otherwise and exited */
            if (!cpu_engine_quickcheck(r->engine)) pz_die("the CPU engine '%s' gave a wrong result in the quick check: use another --isa", o.isa);
        }
    }

    r->prio_gpu = o.prio_gpu;
    r->gpu_boost = o.prio_gpu == PRIO_YES;
    r->limit_pct = o.limit > 0 && o.limit < 100 ? o.limit : 0;
    if (r->limit_pct && o.retune) pz_die("--retune measures the machine at full load: it cannot be combined with --limit");
    profile_apply(r, o.retune, o.no_tune);   /* CPU threads and GPUs: the measured best of this machine unless the user chose (nothing under --limit) */
    const int thr_full = r->nthreads;
    run_limit_setup(r);
    if (r->gpu_boost && !pz_boost_possible()) {
        pz_log(PZ_LOG_WARN, "high priority for the threads that serve the GPUs: the system does not allow it for this user (on Linux it needs root or CAP_SYS_NICE): staying normal");
        r->gpu_boost = false;
    }
    gpu_set_boost(r->use_gpu && r->gpu_boost);
    if (!o.no_tune && !pz_env_on("PZ_NO_PROFILE")) profile_rate_setup(r);

    char idb[17], rs[80], re[80], rsz[80], addr[80];
    seed_to_id(r->cfg.seed, idb);
    u256_to_hex(&r->cfg.range_start, rs);
    u256_to_hex(&r->cfg.range_end, re);
    u256 sz = cfg_range_size(&r->cfg);
    u256_to_dec(&sz, rsz);
    char rsz_short[64];
    size_text(&sz, rsz, rsz_short, sizeof rsz_short);
    pz_strlcpy(addr, r->cfg.address, sizeof addr);
    char memb[32];
    pz_fmt_bytes((double)r->sys.mem_total, memb, sizeof memb);
    pz_log(PZ_LOG_INFO, "pzsolve %s - campaign '%s' %s", PZ_VERSION, r->cfg.name, resuming ? "(resumed)" : "(new)");
    pz_log(PZ_LOG_INFO, "target: puzzle #%d  address %s", r->cfg.puzzle, addr);
    pz_log(PZ_LOG_INFO, "range: 0x%s - 0x%s (%s)", rs, re, rsz_short);
    if (r->cfg.mode == MODE_LINEAR)
        pz_log(PZ_LOG_INFO, "mode: linear | seed id %s | order: blocks of 2^%u keys permuted with the seed", idb, r->cfg.chunk_bits);
    else if (r->cfg.mode == MODE_BSGS)
        pz_log(PZ_LOG_INFO, "mode: bsgs (known public key) | seed id %s | random order of the giant steps determined by the seed", idb);
    else
        pz_log(PZ_LOG_INFO, "mode: kangaroo (known public key) | seed id %s | the seed determines jumps and starting positions", idb);
    if (r->cfg.part[0]) {
        char sd[65];
        pz_hex_encode(r->cfg.seed, 32, sd);
        pz_log(PZ_LOG_INFO, "part %s: this computer scans only its shares of the blocks; the other computers need the same range, --chunk-bits and this seed (-s %s) with the other shares", r->cfg.part, sd);
    }
    pz_log(PZ_LOG_INFO, "hardware: %s, %d usable logical thread%s (%d physical core%s), RAM %s", r->sys.model, r->sys.ncpu, PLURAL(r->sys.ncpu), r->sys.ncore, PLURAL(r->sys.ncore), memb);
    if (r->use_cpu && r->engine) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, engine %s (%d lane%s)", r->nthreads, PLURAL(r->nthreads), r->engine->name, r->engine->lanes, PLURAL(r->engine->lanes));
    else if (r->use_cpu && r->cfg.mode == MODE_KANGAROO) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, kangaroo steps with %s", r->nthreads, PLURAL(r->nthreads), kang_round_name());
    else if (r->use_cpu) pz_log(PZ_LOG_INFO, "CPU: %d thread%s, giant steps with %s", r->nthreads, PLURAL(r->nthreads), bsgs_engine_name());
    if (r->limit_pct) {
        char cpu[120] = "", gpu[80] = "";
        if (r->use_cpu) snprintf(cpu, sizeof cpu, "CPU %d thread%s out of %d (one per physical core first)", r->nthreads, PLURAL(r->nthreads), thr_full);
        if (r->use_gpu) snprintf(gpu, sizeof gpu, "%sGPUs at %d%% of the work-items", r->use_cpu ? ", " : "", r->limit_pct);
        pz_log(PZ_LOG_INFO, "limit %d%%: %s%s; everything always at full speed, without pauses (less heat and power, speed reduced accordingly)", r->limit_pct, cpu, gpu);
    }
    if (r->use_gpu && r->gpu_boost) pz_log(PZ_LOG_INFO, "GPU: the threads that serve the GPUs run at high priority%s", o.prio_gpu == PRIO_YES ? " (--prio-gpu yes)" : " (from the machine profile)");
    if (r->node) pz_log(PZ_LOG_INFO, "node %u: kangaroo starting points differ from those of the other computers on the same campaign", r->node);
    pz_log(PZ_LOG_INFO, "campaign folder: %s", r->cfg.dir);

    run_session_load(r);
    r->sessions++;
    r->t_start = pz_mono();
    run_install_signals(r);
    if (o.keep_awake && !pz_prevent_sleep(true)) pz_log(PZ_LOG_WARN, "--keep-awake: not supported on this system (on Linux use 'systemd-inhibit pzsolve hunt ...')");

    int rc = r->cfg.mode == MODE_KANGAROO ? run_kangaroo(r) : r->cfg.mode == MODE_BSGS ? run_bsgs(r) : run_linear(r);

    double el = run_session_elapsed(r);
    if (r->checkpoint && r->checkpoint(r) != 0) {
        /* the progress of this session may not be on disk: say so loudly and fail the command */
        pz_log(PZ_LOG_ERR, "final state save FAILED in %s (disk full or not writable?): the progress of the last session may be lost", r->cfg.dir);
        rc = 1;
    }
    progress_info pi;
    run_final_progress(r, &pi);
    run_write_report_files(r, &pi, true);
    pz_interrupt_done();   /* state and reports are on disk: a pending console close may terminate the process */
    run_ui_final(r, &pi);
    const bool finished = r->sched && sched_finished(r->sched);
    if (r->cleanup) r->cleanup(r);
    if (r->sched) { sched_free(r->sched); r->sched = NULL; }
    char ds[48];
    pz_log(PZ_LOG_INFO, "session ended after %s", pz_fmt_duration(el, ds, sizeof ds));
    if (atomic_load(&r->found)) {
        char hx[80], wif[80];
        u8 kb[32];
        u256_to_hex(&r->found_key, hx);
        u256_to_be(&r->found_key, kb);
        privkey_to_wif(kb, true, wif, sizeof wif);
        print_found_banner(r, addr, hx, wif);
        pz_log_set_file(NULL);
        free(r);
        return 0;
    }
    if (finished) {
        if (r->cfg.part[0]) {
            printf("\nPart %s was scanned in full without finding the key: the shares of the other computers remain.\n", r->cfg.part);
        } else {
            printf("\nCampaign completed: the whole range was covered without finding the key.\n");
            printf("(Check that the puzzle/address are correct; the computation was tested with 'pzsolve selftest'.)\n");
        }
    } else {
        char byp[32];
        snprintf(byp, sizeof byp, "p%d", r->cfg.puzzle);
        if (r->cfg.puzzle && !strcmp(r->cfg.name, byp))
            printf("\nCampaign interrupted. Resume with:  pzsolve hunt -p %d%s%s\n", r->cfg.puzzle, strcmp(o.dir, "campaigns") ? " -d " : "", strcmp(o.dir, "campaigns") ? o.dir : "");
        else
            printf("\nCampaign interrupted. Resume with:  pzsolve hunt -c %s%s%s\n", r->cfg.name, strcmp(o.dir, "campaigns") ? " -d " : "", strcmp(o.dir, "campaigns") ? o.dir : "");
    }
    pz_log_set_file(NULL);
    free(r);
    return rc;
}

static int cmd_info(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    if (!o.campaign[0] && o.puzzle) snprintf(o.campaign, sizeof o.campaign, "p%d", o.puzzle);
    if (!o.campaign[0]) pz_die("specify the campaign with -c NAME or -p N");
    char p[700];
    snprintf(p, sizeof p, "%s/%s/report.txt", o.dir, o.campaign);
    char *t = pz_read_file(p, NULL);
    if (t) {
        fputs(t, stdout);
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

static int cmd_list(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    campaigns_list(o.dir);
    return 0;
}

static int cmd_merge(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    pz_log_set_quiet(o.quiet);
    if (!o.campaign[0] && o.puzzle) snprintf(o.campaign, sizeof o.campaign, "p%d", o.puzzle);
    if (!o.campaign[0] || !o.nfiles) pz_die("usage: pzsolve merge (-c NAME | -p N) [-d DIR] -f FILE_OR_FOLDER [-f ...]  (the files are the dp.bin of the other computers)");
    char cdir[600], conf[700], err[256];
    snprintf(cdir, sizeof cdir, "%s/%s", o.dir, o.campaign);
    snprintf(conf, sizeof conf, "%s/campaign.conf", cdir);
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    if (cfg_load(&r->cfg, conf, err, sizeof err) != 0) pz_die("%s", err);
    if (r->cfg.mode != MODE_KANGAROO) pz_die("campaign '%s' does not use the kangaroo: 'merge' merges the distinguished points of the kangaroo", o.campaign);
    pz_strlcpy(r->cfg.dir, cdir, sizeof r->cfg.dir);
    run_paths(r);
    if (file_exists(r->path_found)) {
        char *t = pz_read_file(r->path_found, NULL);
        printf("This campaign has already found the key (%s):\n\n%s\n", r->path_found, t ? t : "");
        free(t);
        free(r);
        return 0;
    }
    campaign_lock(cdir);   /* a campaign that is running must be stopped first: both would write the same log */
    if (o.mem_gb > 0) r->mem_limit = (u64)(o.mem_gb * 1073741824.0);
    const char *fl[16];
    for (int i = 0; i < o.nfiles; i++) fl[i] = o.files[i];
    int rc = kangaroo_merge(r, fl, o.nfiles);
    if (rc == 10 || atomic_load(&r->found)) {
        char hx[80], wif[80], addr[80];
        u8 kb[32];
        u256_to_hex(&r->found_key, hx);
        u256_to_be(&r->found_key, kb);
        privkey_to_wif(kb, true, wif, sizeof wif);
        if (r->cfg.address[0]) pz_strlcpy(addr, r->cfg.address, sizeof addr);
        else hash160_to_address(r->cfg.target_h160, addr, sizeof addr);
        print_found_banner(r, addr, hx, wif);
        free(r);
        return 0;
    }
    if (rc == 0) pz_fputs_wrap(stdout, "\nNo collision between the points yet: continue with 'hunt' on this campaign.\n", PZ_LINE_MAX, 2);
    else pz_fputs_wrap(stdout, "\nSome of the files could not be merged (see above).\n", PZ_LINE_MAX, 2);
    free(r);
    return rc;
}

/* A speed such as 800, 120M or 1.5G is not parsed (no floating point): whole numbers with an optional k, M or G. */
static bool parse_speed(const char *t, u64 *out)
{
    size_t n = strlen(t);
    u64 mul = 1;
    char num[40];
    if (n == 0 || n >= sizeof num) return false;
    memcpy(num, t, n + 1);
    if (num[n - 1] == 'k' || num[n - 1] == 'K') mul = 1000;
    else if (num[n - 1] == 'M') mul = 1000000ULL;
    else if (num[n - 1] == 'G') mul = 1000000000ULL;
    if (mul > 1) num[n - 1] = 0;
    u64 v;
    if (!pz_parse_u64(num, &v) || v == 0 || v > ((u64)1 << 62) / mul) return false;
    *out = v * mul;
    return true;
}

static int cmd_split(int argc, char **argv)
{
    if (argc < 3) pz_die("usage: pzsolve split SPEED_1 SPEED_2 [...]   (whole numbers, optionally with k, M or G: for example 800M 120M)");
    const unsigned n = (unsigned)(argc - 1);
    if (n > 64) pz_die("at most 64 computers");
    u64 speed[64];
    unsigned counts[64];
    for (unsigned i = 0; i < n; i++)
        if (!parse_speed(argv[i + 1], &speed[i])) pz_die("invalid speed '%s' (positive whole number, optionally with k, M or G)", argv[i + 1]);
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
    printf("On every computer use the same puzzle or range (-p or -r), the same -s and --chunk-bits, and -m linear.\n");
    return 0;
}

static int cmd_where(int argc, char **argv)
{
    opts_t o;
    parse_opts(argc, argv, &o);
    if (!o.campaign[0] && o.puzzle) snprintf(o.campaign, sizeof o.campaign, "p%d", o.puzzle);
    if (!o.campaign[0] || !o.key[0]) pz_die("usage: pzsolve where (-c NAME | -p N) --key HEX_KEY");
    campaign_cfg c;
    char p[700], err[256];
    snprintf(p, sizeof p, "%s/%s/campaign.conf", o.dir, o.campaign);
    if (cfg_load(&c, p, err, sizeof err) != 0) pz_die("%s", err);
    u256 key;
    if (!u256_from_hex(&key, o.key)) pz_die("invalid key (hexadecimal digits expected)");
    if (c.mode == MODE_KANGAROO) pz_die("the campaign uses the kangaroo: it has no predetermined visit order, 'where' does not apply");
    if (c.mode == MODE_BSGS) {
        char berr[200];
        bsgs_params bp;
        if (!bsgs_params_init(&bp, &c.range_start, &c.range_end, &c.pubkey, c.bsgs_t, berr, sizeof berr)) pz_die("%s", berr);
        u64 gi = bsgs_giant_index(&bp, &key);
        if (!gi) pz_die("the key is not in the campaign range");
        campaign_cfg sc;
        bsgs_sched_cfg(&bp, &c, &sc);
        sched_t *s = sched_new(&sc);
        u256 seq;
        u64 off;
        u256 gk = u256_from_u64(gi);
        if (!sched_locate(s, &gk, &seq, &off)) pz_die("giant index out of range");
        char sb[80];
        const long double nblocks = u256_to_ld(&s->blocks_all);
        char g1[48], g2[48], g3[48], g4[48];
        snprintf(g1, sizeof g1, "%llu", (unsigned long long)gi);
        snprintf(g2, sizeof g2, "%llu", (unsigned long long)bp.Ng);
        snprintf(g4, sizeof g4, "%.0Lf", nblocks);
        pz_prop_line("giant step", "the key will be found at step %s of %s", pz_group_digits(g1, g3, sizeof g3), pz_group_digits(g2, g1, sizeof g1));
        pz_prop_line("visit order", "block %s of %s, %.4Lf%% of the way", pz_group_digits(u256_to_dec(&seq, sb), g2, sizeof g2), pz_group_digits(g4, g3, sizeof g3),
                     u256_to_ld(&seq) * 100.0L / nblocks);
        sched_free(s);
        return 0;
    }
    sched_t *s = sched_new(&c);
    u256 seq;
    u64 off;
    if (!sched_locate(s, &key, &seq, &off)) pz_die("the key is not in the campaign range");
    char sb[80];
    const long double blocks = u256_to_ld(&s->blocks_all);            /* of the whole search order, also with --part */
    long double frac = u256_to_ld(&seq) / blocks;
    {
        char g1[64], g2[64], g3[64];
        snprintf(g3, sizeof g3, "%.0Lf", blocks);
        pz_prop_line("visit order", "block %s of %s, offset %llu within the block, %.4Lf%% of the way", pz_group_digits(u256_to_dec(&seq, sb), g1, sizeof g1),
                     pz_group_digits(g3, g2, sizeof g2), (unsigned long long)off, frac * 100.0L);
    }
    if (s->part.total) {
        u256 local;
        if (sched_part_position(s, &seq, &local))
            pz_prop_line("part", "in part %s of this computer: its block %s of %.0Lf", s->part_text, u256_to_dec(&local, sb), u256_to_ld(&s->blocks_part));
        else
            pz_prop_line("part", "not in part %s of this computer: another computer will scan it", s->part_text);
    }
    sched_free(s);
    return 0;
}

/* Cost model per method for a range of 2^bits keys (W = 2^bits). Units: group operations / keys. */
static const char *recommended_method(const puzzle_info *p)
{
    if (!p->pubkey[0]) return "linear search (only the address is known)";
    return "kangaroo (public key known)";
}

static void print_puzzle_effort(const puzzle_info *p, double rate_lin, double rate_kang, bool measured)
{
    const int n = p->n;
    char r1[32], r2[32], t1[48], t2[48], t3[48];
    if (rate_lin > 0)
        pz_prop_line("speed used", "%s keys/s (linear)%s%s%s; %s", pz_fmt_si(rate_lin, r1, sizeof r1), rate_kang > 0 && p->pubkey[0] ? ", " : "",
                     p->pubkey[0] && rate_kang > 0 ? pz_fmt_si(rate_kang, r2, sizeof r2) : "", p->pubkey[0] && rate_kang > 0 ? " steps/s (kangaroo)" : "",
                     measured ? "measured now on the CPU of this machine" : "given with --rate");
    pz_section("Mean effort");
    const long double W = ldexpl(1.0L, n - 1);
    pz_prop_line("linear search", "%.3Le keys on average (2^%.1f), %.3Le in the worst case", W / 2, (double)(n - 2), W);
    if (rate_lin > 0)
        pz_prop_line("", "time: 50%% in %s, 100%% in %s", pz_fmt_duration((double)(W / 2) / rate_lin, t1, sizeof t1), pz_fmt_duration((double)W / rate_lin, t2, sizeof t2));
    if (p->pubkey[0]) {
        long double st = KANG_C_MEAN * sqrtl(W);
        pz_prop_line("kangaroo", "%.3Le steps on average (2^%.1f)", st, (double)log2l(st));
        if (rate_kang > 0)
            pz_prop_line("", "time: median %s, 90%% in %s", pz_fmt_duration((double)(0.9395L * KANG_C_MEAN * sqrtl(W)) / rate_kang, t1, sizeof t1),
                         pz_fmt_duration((double)(1.7122L * KANG_C_MEAN * sqrtl(W)) / rate_kang, t3, sizeof t3));
        /* BSGS with a RAM table, if one that beats the kangaroo fits in 70% of the free memory */
        sysinfo_t si;
        sysinfo_detect(&si);
        u256 w256 = u256_pow2((unsigned)(n - 1));
        bool viable;
        u64 T = bsgs_choose_T(&w256, (u64)(0.70 * (double)si.mem_avail), &viable);
        if (viable) {
            char mb[48], cs[48];
            pz_fmt_bytes((double)bsgs_bytes_for(T), mb, sizeof mb);
            long double cost = bsgs_mean_cost(&w256, T);
            pz_prop_line("BSGS in RAM", "table of %llu entries (%s), %.3Le operations on average (2^%.1f), deterministic", (unsigned long long)T, mb, cost, (double)log2l(cost));
            if (rate_kang > 0) pz_prop_line("", "time, at the kangaroo speed: %s on average", pz_fmt_duration((double)cost / rate_kang, cs, sizeof cs));
        } else {
            pz_prop_line("BSGS in RAM", "not worthwhile: the best table does not fit in 70%% of the free RAM of this machine");
        }
    }
}

static int cmd_puzzle(int argc, char **argv)
{
    if (argc < 3) pz_die("usage: pzsolve puzzle N [--rate KEYS_PER_SECOND]");
    int n = (int)opt_int("puzzle", argv[2], 1, 256);
    double rate_lin = -1, rate_kang = -1;
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--rate")) {
            if (i + 1 >= argc) pz_die("--rate requires a value (keys per second, for example --rate 800M or --rate 1.5G)");
            rate_lin = rate_kang = opt_rate("--rate", argv[++i]);
        }
        else pz_die("unknown option: %s", argv[i]);
    u256 a, b;
    if (!puzzle_range(n, &a, &b)) pz_die("puzzle out of range (1..256)");
    char ha[80], hb[80], ds[100];
    u256_to_hex(&a, ha);
    u256_to_hex(&b, hb);
    u256 sz = cfg_range_size(&(campaign_cfg){.range_start = a, .range_end = b});
    u256_to_dec(&sz, ds);
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
    pz_prop_line("range", "0x%s to 0x%s", ha, hb);
    pz_prop_line("keys", "%s (2^%d)", pz_group_digits(ds, dsg, sizeof dsg), n - 1);
    if (p) {
        pz_prop_line("best method", "%s", recommended_method(p));
        bool measured = rate_lin < 0;
        if (measured) {
            const bool tty = pz_stderr_is_terminal();
            fprintf(stderr, tty ? "measuring the speed of this machine (about 4 seconds)...\r" : "measuring the speed of this machine (about 4 seconds)...\n");
            rate_lin = bench_cpu_rate(1.0);
            rate_kang = p->pubkey[0] ? kangaroo_bench(2.0, false) : 0;
            if (tty) fprintf(stderr, "\x1b[K");
        }
        print_puzzle_effort(p, rate_lin, rate_kang, measured);
        printf("\nGPUs, if present, add to the CPU: give the overall speed with --rate.\n");
        printf("To start the campaign: pzsolve hunt -p %d\n", n);
    } else {
        long double w = ldexpl(1.0L, n - 1);
        pz_section("Mean effort");
        pz_prop_line("linear search", "%.3Le keys in the worst case, %.3Le on average (no public key)", w, w / 2);
        pz_prop_line("kangaroo", "about %.3Le steps on average (with the public key)", KANG_C_MEAN * sqrtl(w));
    }
    return 0;
}

static int cmd_puzzles(int argc, char **argv)
{
    bool only_open = false, only_solved = false, only_pub = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--open") || !strcmp(argv[i], "--aperti")) only_open = true;           /* "--aperti", "--risolti": former spellings */
        else if (!strcmp(argv[i], "--solved") || !strcmp(argv[i], "--risolti")) only_solved = true;
        else if (!strcmp(argv[i], "--pubkey")) only_pub = true;
        else pz_die("unknown option: %s (valid: --open, --solved, --pubkey)", argv[i]);
    }
    if (only_open && only_solved) pz_die("--open and --solved are mutually exclusive");
    printf("%4s  %-9s %-6s %-36s %s\n", "N", "status", "pubkey", "address", "mean effort (method)");
    int shown = 0, nsolved = 0, nopen = 0;
    for (int i = 0; i < PZ_NUM_PUZZLES; i++) {
        const puzzle_info *p = &PZ_PUZZLES[i];
        bool solved = p->privkey[0] != 0;
        if (solved) nsolved++; else nopen++;
        if ((only_open && solved) || (only_solved && !solved) || (only_pub && !p->pubkey[0])) continue;
        double eff = p->pubkey[0] ? (p->n - 1) / 2.0 + log2(KANG_C_MEAN) : (p->n - 2);
        printf("%4d  %-9s %-6s %-36s 2^%.1f %s\n", p->n, solved ? "solved" : "OPEN", p->pubkey[0] ? "yes" : "no", p->address, eff,
               p->pubkey[0] ? "steps (kangaroo)" : "keys (linear)");
        shown++;
    }
    printf("\n%d puzzles shown out of %d (%d solved, %d open). The database is verified by 'selftest'.\n", shown, PZ_NUM_PUZZLES, nsolved, nopen);
    printf("Puzzles 161..256 existed in the original 2015 transaction but the creator withdrew their funds in 2017:\n"
           "they are not listed here; 'pzsolve puzzle N' shows their address.\n");
    return 0;
}

static int cmd_devices(void)
{
    sysinfo_t s;
    sysinfo_detect(&s);
    char m1[32], m2[32], c2[32], c3[32];
    pz_section("CPU");
    pz_prop_line("model", "%s", s.model);
    pz_prop_line("threads", "%d usable logical thread%s on %d physical core%s", s.ncpu, PLURAL(s.ncpu), s.ncore, PLURAL(s.ncore));
    if (s.l2_kib > 0) pz_prop_line("caches", "L2 %s, L3 %s", pz_fmt_bytes((double)s.l2_kib * 1024, c2, sizeof c2), pz_fmt_bytes((double)s.l3_kib * 1024, c3, sizeof c3));
    pz_prop_line("RAM", "%s total, %s available", pz_fmt_bytes((double)s.mem_total, m1, sizeof m1), pz_fmt_bytes((double)s.mem_avail, m2, sizeof m2));
    pz_section("CPU engines");
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    for (int i = 0; i < n; i++) printf("  %-10s%2d lanes   %s\n", all[i]->name, all[i]->lanes, all[i]->supported() ? "available" : "not supported by this CPU");
    pz_section("GPUs (CUDA and OpenCL)");
    gpu_list_devices();
    return 0;
}

int main(int argc, char **argv)
{
    pz_console_init();
    if (argc < 2) {
        usage_main(stderr);
        if (pz_console_is_own()) {
            /* double-clicked in Explorer: the window would vanish before the text could be read */
            fprintf(stderr, "\nThis is a command-line program: open PowerShell (or the Command Prompt) in its folder\n"
                            "and run for example  .\\pzsolve.exe puzzles --open\n\nPress Enter to close...");
            getchar();
        }
        return 2;
    }
    const char *cmd = argv[1];
    g_cmd = cmd;
    const bool noargs = !strcmp(cmd, "devices") || !strcmp(cmd, "selftest") || !strcmp(cmd, "version");
    if (noargs && argc > 2) pz_die("the command '%s' takes no arguments ('%s' not recognized)", cmd, argv[2]);
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
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        printf("pzsolve %s (%s)\n", PZ_VERSION, PZ_BUILD_INFO);
        return 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { pz_help(argc > 2 ? argv[2] : NULL, stdout); return 0; }
    pz_die("unknown command '%s' (see 'pzsolve help')", cmd);
    return 2;
}
