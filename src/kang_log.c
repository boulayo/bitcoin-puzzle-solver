/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_log.c - the file of distinguished points of a kangaroo campaign (dp.bin): creation, loading, appending with recovery from
 * write errors, and the merge of another computer's file. The table it feeds is in kangaroo.c. */
#include "kangaroo_priv.h"
#include <pzcore/hash.h>
#include "platform.h"
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#define KANG_LOG_BUF_MAX ((size_t)1 << 22)      /* records the log buffer may grow to while the disk refuses writes (128 MiB) */

/* Fingerprint of the campaign in a DP log: base, W and public key. */
static void log_fingerprint(const kang_state *ks, const pz_u256 *base, const char *tag, u8 fp[32])
{
    u8 buf[32 * 3 + 33];
    pz_u256_to_be(base, buf);
    pz_u256_to_be(&ks->W, buf + 32);
    memcpy(buf + 64, tag, 8);
    memcpy(buf + 72, ks->r->cfg.pubkey33, 33);
    pz_sha256(buf, 72 + 33, fp);
}

/* wmu held. A write failed (disk full, quota, I/O error): what reached the file is unknown, maybe a torn record, maybe whole ones. Reopens
 * the file, cuts it back to whole records and drops from the block the records that did get there, so the next write appends exactly
 * the rest. */
static int log_resync(kang_state *ks)
{
    if (ks->log) { fclose(ks->log); ks->log = NULL; }
    FILE *f = fopen(ks->log_path, "rb");
    if (!f) return -1;
    const off_t size = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : -1;
    fclose(f);
    if (size < (off_t)ks->log_hdr) return -1;
    const u64 whole = (u64)(size - ks->log_hdr) / sizeof(dp_rec);
    if (pz_truncate_path(ks->log_path, ks->log_hdr + whole * sizeof(dp_rec)) != 0) return -1;
    const u64 on_file = whole > ks->log_recs ? whole - ks->log_recs : 0;      /* of the records of the block, already written */
    const size_t drop = on_file > ks->nw ? ks->nw : (size_t)on_file;
    memmove(ks->wbuf, ks->wbuf + drop, (ks->nw - drop) * sizeof(dp_rec));
    ks->nw -= drop;
    ks->log_recs = whole;
    ks->log = fopen(ks->log_path, "ab");
    if (!ks->log) return -1;
    ks->log_broken = false;
    return 0;
}

/* wmu held. Writes the block. 0 when it is in the file; -1 when the file refuses, and then the block stays, for the next try. */
static int log_write_block(kang_state *ks)
{
    if (!ks->log && !ks->log_broken) { ks->nw = 0; return 0; }          /* no log at all (test states) */
    if (ks->log_broken && log_resync(ks) != 0) return -1;
    if (ks->nw && (fwrite(ks->wbuf, sizeof(dp_rec), ks->nw, ks->log) != ks->nw || fflush(ks->log) != 0)) {
        ks->log_broken = true;
        return -1;
    }
    if (ks->nw) {
        ks->log_recs += ks->nw;
        ks->nw = 0;
        atomic_store(&ks->log_warned, false);
        if (ks->capw > KANG_LOG_BUF_INIT) {                              /* it grew while the disk refused: back to the usual size */
            ks->wbuf = pz_xrealloc(ks->wbuf, KANG_LOG_BUF_INIT * sizeof(dp_rec));
            ks->capw = KANG_LOG_BUF_INIT;
        }
    }
    return 0;
}

/* mu and wmu held, nw == 0: the records collected so far become the block to write, an empty buffer takes the place of logbuf. */
static void log_take_block(kang_state *ks)
{
    dp_rec *b = ks->logbuf;
    const size_t c = ks->caplog;
    ks->logbuf = ks->wbuf;
    ks->caplog = ks->capw;
    ks->wbuf = b;
    ks->capw = c;
    ks->nw = ks->nlog;
    ks->nlog = 0;
}

int kang_flush_log(kang_state *ks)
{
    for (;;) {
        pthread_mutex_lock(&ks->wmu);                                    /* waits for a write that is going on; the table is not locked meanwhile */
        const int rc = log_write_block(ks);                              /* a block taken out earlier and not in the file yet goes first */
        pthread_mutex_unlock(&ks->wmu);
        if (rc != 0) return -1;
        pthread_mutex_lock(&ks->mu);
        if (pthread_mutex_trylock(&ks->wmu) != 0) {                      /* another write has started since: wait for it, without mu */
            pthread_mutex_unlock(&ks->mu);
            continue;
        }
        if (ks->nw) {                                                    /* so has a block been taken out: that one first */
            pthread_mutex_unlock(&ks->wmu);
            pthread_mutex_unlock(&ks->mu);
            continue;
        }
        if (ks->nlog) log_take_block(ks);
        pthread_mutex_unlock(&ks->mu);                                   /* the write itself needs only wmu */
        const int wr = log_write_block(ks);                              /* flushed there */
        pthread_mutex_unlock(&ks->wmu);
        return wr;
    }
}

/* mu held, logbuf full. Takes the block out for writing, or, when the file refuses it, enlarges logbuf (up to KANG_LOG_BUF_MAX records
 * in all); false when there is no room for one more record. When a write is going on (the other buffer is full too: the disk cannot
 * keep up) it waits for that write, with mu held, so the inserting threads wait as well and no point is lost. */
static bool log_make_room(kang_state *ks)
{
    pthread_mutex_lock(&ks->wmu);
    if (ks->nw) log_write_block(ks);                                     /* one that was taken out and not written yet, or refused: it goes first */
    bool room = true;
    if (!ks->nw) {
        log_take_block(ks);
        ks->log_ready = true;
    } else if (ks->caplog + ks->nw < KANG_LOG_BUF_MAX) {
        ks->caplog *= 2;
        ks->logbuf = pz_xrealloc(ks->logbuf, ks->caplog * sizeof(dp_rec));
    } else {
        room = false;
    }
    pthread_mutex_unlock(&ks->wmu);
    return room;
}

/* mu held. Appends to the log buffer. When it is full it is exchanged for the empty one, and the thread that unlocks mu next writes it
 * (kang_table_unlock). When the disk refuses, the buffer grows (up to KANG_LOG_BUF_MAX records) instead of overflowing, and the next
 * checkpoint tries again; beyond that new points stay in RAM only (false: the table keeps a copy of the record, in spill). *idx is the index
 * of the record in the log. */
bool kang_log_push(kang_state *ks, const dp_rec *rec, u64 *idx)
{
    if (ks->nlog == ks->caplog && !log_make_room(ks)) {
        atomic_fetch_add(&ks->dropped, 1);
        if (!atomic_exchange(&ks->log_warned, true))
            pz_log(PZ_LOG_ERR, "writing %s failed (disk full?): new distinguished points stay in RAM only and will be lost if the program stops", ks->log_path);
        return false;
    }
    ks->logbuf[ks->nlog++] = *rec;
    *idx = ks->log_next++;
    return true;
}

/* mu held. The record at index idx of the log: the last nlog records are in logbuf, the nw before them in wbuf (being written, or refused by
 * the disk), the others in the file. wmu (taken after mu, as everywhere) keeps wbuf and the file still meanwhile: a write that is going on
 * is waited for. The file is read with a stream of its own, which sees what the writes flushed. */
bool kang_log_get(kang_state *ks, u64 idx, dp_rec *out)
{
    if (idx >= ks->log_next) return false;
    const u64 in_logbuf = ks->log_next - ks->nlog;
    if (idx >= in_logbuf) { *out = ks->logbuf[idx - in_logbuf]; return true; }
    pthread_mutex_lock(&ks->wmu);
    const u64 in_wbuf = in_logbuf - ks->nw;
    bool ok = false;
    if (idx >= in_wbuf) {
        *out = ks->wbuf[idx - in_wbuf];
        ok = true;
    } else {
        FILE *f = fopen(ks->log_path, "rb");
        if (f) {
            ok = fseeko(f, (off_t)(ks->log_hdr + idx * sizeof(dp_rec)), SEEK_SET) == 0 && fread(out, sizeof *out, 1, f) == 1;
            fclose(f);
        }
    }
    pthread_mutex_unlock(&ks->wmu);
    return ok;
}

void kang_table_unlock(kang_state *ks)
{
    const bool write = ks->log_ready;
    ks->log_ready = false;
    pthread_mutex_unlock(&ks->mu);
    if (!write) return;
    pthread_mutex_lock(&ks->wmu);
    log_write_block(ks);                                                 /* a failure leaves the block for the next try; the checkpoint reports it */
    pthread_mutex_unlock(&ks->wmu);
}

/* Fingerprint of the logs of this campaign (see KANG_LOG_MAGIC). */
void kang_log_fp(const kang_state *ks, u8 fp[32]) { log_fingerprint(ks, &ks->base, "PZDPLOG4", fp); }

/* The header of a log of this campaign: KANG_LOG_HDR bytes, or KANG_LOG_HDR4 for version 4. */
static void log_header(const kang_state *ks, const u8 fp[32], bool v4, u8 hdr[KANG_LOG_HDR])
{
    memcpy(hdr, v4 ? KANG_LOG_MAGIC4 : KANG_LOG_MAGIC, 6);
    memcpy(hdr + 6, fp, 32);
    if (!v4) memcpy(hdr + 38, ks->jump_tag, 4);
}

/* Creates the file with its header. NULL when the header could not be written (what reached the file is the beginning of a header, which
 * the next start recognizes as an interrupted creation). */
FILE *kang_log_create(kang_state *ks, const char *path, const u8 fp[32])
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        pz_log(PZ_LOG_ERR, "cannot create %s: %s", path, strerror(errno));
        return NULL;
    }
    u8 hdr[KANG_LOG_HDR];
    log_header(ks, fp, false, hdr);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr || fflush(f) != 0) {
        const int e = errno;
        fclose(f);
        pz_log(PZ_LOG_ERR, "cannot write the header of %s: %s", path, strerror(e));
        return NULL;
    }
    ks->log_hdr = KANG_LOG_HDR;
    return f;
}

int kang_log_class(const u8 magic[6])
{
    if (memcmp(magic, "PZDP", 4) != 0) return KANG_LOG_FOREIGN;
    if (memcmp(magic, KANG_LOG_MAGIC, 6) == 0) return KANG_LOG_CURRENT;
    if (memcmp(magic, KANG_LOG_MAGIC4, 6) == 0) return KANG_LOG_V4;
    return magic[4] >= '1' && magic[4] <= '3' ? KANG_LOG_OLDER : KANG_LOG_NEWER;
}

/* Reads the records of an open log, positioned after the header, into the table; stops at the end of the file or at the record that solves
 * the key. Returns the number of records read, or -1 on a read error (what was read stays in the table). A record cut short by the end of
 * the file is not counted. */
long long kang_log_read(kang_state *ks, FILE *f)
{
    dp_rec rec;
    long long n = 0;
    pthread_mutex_lock(&ks->mu);
    while (fread(&rec, sizeof rec, 1, f) == 1) {
        int rc = kang_table_insert(ks, &rec, (u64)n);
        n++;
        if (rc == 2) break;
    }
    const bool failed = ferror(f);                       /* a failed read is not the end of the file */
    const int e = errno;
    kang_table_unlock(ks);
    errno = e;
    return failed ? -1 : n;
}

static int log_unreadable(const kang_state *ks, int err)
{
    pz_log(PZ_LOG_ERR, "cannot read %s: %s (the file is left as it is)", ks->log_path, err ? strerror(err) : "read error");
    return -1;
}

static int log_foreign(const kang_state *ks)
{
    pz_log(PZ_LOG_ERR, "file %s does not belong to this campaign (different header); it is left as it is: move it away to start the distinguished "
           "points again", ks->log_path);
    return -1;
}

void kang_jump_id(const u8 tag[4], char out[9]) { snprintf(out, 9, "%02x%02x%02x%02x", tag[0], tag[1], tag[2], tag[3]); }

/* Reads and judges the header of an open log, from its start: KANG_HDR_OK leaves the file after the header (*hlen bytes); *size is the size of
 * the file and hdr holds what was read (the magic, and the jump table of a log of KANG_HDR_OTHER_JUMPS). A read error keeps errno. The magic
 * says how long the header is; a file shorter than that is looked at as it is: a creation that did not finish (disk full, power cut) leaves
 * nothing or the beginning of the header (KANG_HDR_SHORT, a file that holds no point), and any other short file is not ours. */
static int log_read_header(const kang_state *ks, FILE *f, const u8 fp[32], off_t *size, size_t *hlen, u8 hdr[KANG_LOG_HDR])
{
    errno = 0;
    *size = -1;
    *hlen = KANG_LOG_HDR;
    if (fseeko(f, 0, SEEK_END) == 0) *size = ftello(f);
    if (*size < 0 || fseeko(f, 0, SEEK_SET) != 0) return KANG_HDR_READ_ERR;
    const size_t got = *size < 6 ? (size_t)*size : 6;
    if (fread(hdr, 1, got, f) != got) return KANG_HDR_READ_ERR;
    const int cls = *size < 6 ? KANG_LOG_CURRENT : kang_log_class(hdr);
    u8 want[KANG_LOG_HDR];
    *hlen = cls == KANG_LOG_V4 ? KANG_LOG_HDR4 : KANG_LOG_HDR;
    log_header(ks, fp, cls == KANG_LOG_V4, want);
    if (*size < (off_t)*hlen && (cls == KANG_LOG_CURRENT || cls == KANG_LOG_V4 || *size < KANG_LOG_HDR4)) {
        if (fread(hdr + got, 1, (size_t)*size - got, f) != (size_t)*size - got) return KANG_HDR_READ_ERR;
        return memcmp(hdr, want, (size_t)*size) != 0 ? KANG_HDR_FOREIGN : KANG_HDR_SHORT;
    }
    if (cls == KANG_LOG_OLDER) return KANG_HDR_OLDER;
    if (cls == KANG_LOG_NEWER) return KANG_HDR_NEWER;
    if (cls == KANG_LOG_FOREIGN) return KANG_HDR_FOREIGN;
    if (fread(hdr + 6, 1, *hlen - 6, f) != *hlen - 6) return KANG_HDR_READ_ERR;
    if (memcmp(hdr + 6, fp, 32) != 0) return KANG_HDR_FOREIGN;
    return memcmp(hdr, want, *hlen) != 0 ? KANG_HDR_OTHER_JUMPS : KANG_HDR_OK;
}

/* A new log for the campaign: its records start at index 0, and the entries of the table refer to them. 1, or -1 when it cannot be created. */
static int log_start(kang_state *ks, const u8 fp[32])
{
    ks->log = kang_log_create(ks, ks->log_path, fp);
    ks->log_recs = ks->log_next = 0;
    ks->log_refs = ks->log != NULL;
    return ks->log ? 1 : -1;
}

int kang_load_log(kang_state *ks)
{
    FILE *f = fopen(ks->log_path, "rb");
    const int open_err = f ? 0 : errno;
    u8 fp[32];
    kang_log_fp(ks, fp);
    if (!f) {
        /* only a file that is not there is made anew: any other failure (permissions, I/O error, too many open files) may be passing,
         * and creating the file would empty it */
        if (open_err != ENOENT) return log_unreadable(ks, open_err);
        return log_start(ks, fp);
    }
    off_t size;
    size_t hlen;
    u8 hdr[KANG_LOG_HDR];
    const int hc = log_read_header(ks, f, fp, &size, &hlen, hdr);
    const int e = errno;
    if (hc != KANG_HDR_OK) fclose(f);
    if (hc == KANG_HDR_READ_ERR) return log_unreadable(ks, e);
    if (hc == KANG_HDR_FOREIGN) return log_foreign(ks);
    if (hc == KANG_HDR_SHORT) {
        pz_log(PZ_LOG_WARN, "distinguished points: %s is shorter than its header (%lld of %zu bytes: an interrupted creation, disk full?); starting a new file",
               ks->log_path, (long long)size, hlen);
        return log_start(ks, fp);
    }
    if (hc == KANG_HDR_OLDER) {
        /* A log of an older layout of the walkers (versions up to 1.9.0 chose it for campaigns with fewer than 256 kangaroos, that is
         * small intervals): its points cannot be used. It is kept under another name and the campaign goes on with a new log. */
        char old[640];
        snprintf(old, sizeof old, "%s.v%c", ks->log_path, hdr[4]);
        if (rename(ks->log_path, old) != 0) {
            pz_log(PZ_LOG_ERR, "file %s was written by an older layout of the kangaroo and cannot be used, and it could not be moved away", ks->log_path);
            return -1;
        }
        pz_log(PZ_LOG_WARN, "distinguished points: %s was written by an older layout of the kangaroo (versions up to 1.9.0, small intervals) and cannot be "
               "used; it is kept as %s and the campaign goes on with a new file", ks->log_path, old);
        return log_start(ks, fp);
    }
    if (hc == KANG_HDR_NEWER) {
        pz_log(PZ_LOG_ERR, "file %s was written by a newer version of pzsolve (%.4s%c) and cannot be used by this one: it is left as it is; use that version, "
               "or move the file away to start the distinguished points again", ks->log_path, (const char *)hdr, isprint(hdr[4]) ? hdr[4] : '?');
        return -1;
    }
    if (hc == KANG_HDR_OTHER_JUMPS) {
        char theirs[9], ours[9];
        kang_jump_id(hdr + 38, theirs);
        kang_jump_id(ks->jump_tag, ours);
        pz_log(PZ_LOG_ERR, "file %s was written with another jump table (id %s, this campaign: %s): another seed, the points cannot meet; it is "
               "left as it is: move it away to start the distinguished points again", ks->log_path, theirs, ours);
        return -1;
    }
    /* the whole records of the file: a record cut short at the end (the program stopped in the middle of a write) is cut off, and only that */
    const u64 rest = (u64)size - hlen, whole = rest / sizeof(dp_rec);
    ks->log_hdr = (unsigned)hlen;                     /* a log of version 4 stays one: it is appended to as it is */
    ks->log_next = whole;                             /* the entries refer to the records of the file by their index (kang_log_get) */
    const long long n = kang_log_read(ks, f);
    const int re = errno;
    fclose(f);
    if (n < 0) return log_unreadable(ks, re);
    if (rest % sizeof(dp_rec) && pz_truncate_path(ks->log_path, hlen + whole * sizeof(dp_rec)) != 0) {
        pz_log(PZ_LOG_ERR, "cannot cut the partly written record off the end of %s: %s", ks->log_path, strerror(errno));
        return -1;
    }
    ks->loaded = (u64)n;
    ks->log_recs = whole;
    ks->log = fopen(ks->log_path, "ab");
    ks->log_refs = ks->log != NULL;
    return ks->log ? 0 : -1;
}

/* Adds the distinguished points of another computer's log of the same campaign (same range and public key; the jump table must be the same
 * too, i.e. the same seed, or the walks of the two computers never meet). Returns what the header says (KANG_HDR_OK when the points were
 * read; KANG_HDR_OTHER_JUMPS puts the id of the file's jump table in st->jump_id), or KANG_HDR_READ_ERR when the file cannot be opened or
 * read. A log of version 4 has no jump table in its header: it is taken as it is. A collision between a tame and a wild point solves the key. */
int kang_merge_file(kang_state *ks, const char *path, kang_merge_stats *st)
{
    FILE *f = fopen(path, "rb");
    if (!f) return KANG_HDR_READ_ERR;
    u8 hdr[KANG_LOG_HDR], fp[32];
    off_t size;
    size_t hlen;
    kang_log_fp(ks, fp);
    const int hc = log_read_header(ks, f, fp, &size, &hlen, hdr);
    if (hc != KANG_HDR_OK) {
        if (hc == KANG_HDR_OTHER_JUMPS) kang_jump_id(hdr + 38, st->jump_id);
        fclose(f);
        return hc;
    }
    dp_rec rec;
    pthread_mutex_lock(&ks->mu);
    while (fread(&rec, sizeof rec, 1, f) == 1) {      /* a partly written last record is ignored */
        st->read++;
        const int rc = kang_table_insert(ks, &rec, KANG_AT_NEW);
        if (rc == 1) st->added++;
        else if (rc == 0) st->dups++;
        else if (rc < 0) st->skipped++;
        else { st->solved = true; break; }
    }
    kang_table_unlock(ks);
    const bool failed = ferror(f);
    fclose(f);
    return failed ? KANG_HDR_READ_ERR : KANG_HDR_OK;  /* a failed read is not the end of the file */
}
