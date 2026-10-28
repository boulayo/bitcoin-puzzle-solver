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
    if (size < 38) return -1;
    const u64 whole = (u64)(size - 38) / sizeof(dp_rec);
    if (truncate(ks->log_path, 38 + (off_t)(whole * sizeof(dp_rec))) != 0) return -1;
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
        int wr = log_write_block(ks);
        if (wr == 0 && ks->log && fflush(ks->log) != 0) { ks->log_broken = true; wr = -1; }
        pthread_mutex_unlock(&ks->wmu);
        return wr;
    }
}

/* mu held, logbuf full. Takes the block out for writing, or, when the file refuses it, enlarges logbuf (up to KANG_LOG_BUF_MAX records
 * in all); false when there is no room for one more record. When a write is going on (the other buffer is full too: the disk cannot
 * keep up) it waits for that write, with mu held, so the inserting threads wait as well and no point is lost. */
static bool log_make_room(kang_state *ks)
{
    if (pthread_mutex_trylock(&ks->wmu) != 0) pthread_mutex_lock(&ks->wmu);
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
 * checkpoint tries again; beyond that new points stay in the table only. */
void kang_log_push(kang_state *ks, const dp_rec *rec)
{
    if (ks->nlog == ks->caplog && !log_make_room(ks)) {
        atomic_fetch_add(&ks->dropped, 1);
        if (!atomic_exchange(&ks->log_warned, true))
            pz_log(PZ_LOG_ERR, "writing %s failed (disk full?): new distinguished points stay in RAM only and will be lost if the program stops", ks->log_path);
        return;
    }
    ks->logbuf[ks->nlog++] = *rec;
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

/* Creates the file with its header. NULL when the header could not be written (what reached the file is the beginning of a header, which
 * the next start recognizes as an interrupted creation). */
FILE *kang_log_create(const kang_state *ks, const char *path, const u8 fp[32])
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        pz_log(PZ_LOG_ERR, "cannot create %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fwrite(KANG_LOG_MAGIC, 1, 6, f) != 6 || fwrite(fp, 1, 32, f) != 32 || fflush(f) != 0) {
        const int e = errno;
        fclose(f);
        pz_log(PZ_LOG_ERR, "cannot write the header of %s: %s", path, strerror(e));
        return NULL;
    }
    return f;
}

int kang_log_class(const u8 magic[6])
{
    if (memcmp(magic, "PZDP", 4) != 0) return KANG_LOG_FOREIGN;
    if (memcmp(magic, KANG_LOG_MAGIC, 6) == 0) return KANG_LOG_CURRENT;
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
        int rc = kang_table_insert(ks, &rec, true);
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
    pz_log(PZ_LOG_ERR, "file %s does not belong to this campaign (different header): ignored", ks->log_path);
    return -1;
}

int kang_load_log(kang_state *ks)
{
    FILE *f = fopen(ks->log_path, "rb");
    u8 fp[32];
    kang_log_fp(ks, fp);
    if (!f) {
        ks->log = kang_log_create(ks, ks->log_path, fp);
        return ks->log ? 1 : -1;
    }
    errno = 0;
    off_t size = -1;
    if (fseeko(f, 0, SEEK_END) == 0) size = ftello(f);
    if (size < 0 || fseeko(f, 0, SEEK_SET) != 0) {
        const int e = errno;
        fclose(f);
        return log_unreadable(ks, e);
    }
    u8 hdr[38];
    if (size < (off_t)sizeof hdr) {
        /* Shorter than the header: a creation that did not finish (disk full, power cut) leaves nothing, or the beginning of the header. That
         * file holds no point: the log starts again. Any other short file is not ours. */
        u8 want[38];
        memcpy(want, KANG_LOG_MAGIC, 6);
        memcpy(want + 6, fp, 32);
        const bool read_ok = fread(hdr, 1, (size_t)size, f) == (size_t)size;
        const int e = errno;
        fclose(f);
        if (!read_ok) return log_unreadable(ks, e);
        if (memcmp(hdr, want, (size_t)size) != 0) return log_foreign(ks);
        pz_log(PZ_LOG_WARN, "distinguished points: %s is shorter than its header (%lld of %zu bytes: an interrupted creation, disk full?); starting a new file",
               ks->log_path, (long long)size, sizeof hdr);
        ks->log = kang_log_create(ks, ks->log_path, fp);
        return ks->log ? 1 : -1;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        const int e = errno;
        fclose(f);
        return log_unreadable(ks, e);
    }
    const int cls = kang_log_class(hdr);
    if (cls == KANG_LOG_OLDER) {
        /* A log of an older layout of the walkers (versions up to 1.9.0 chose it for campaigns with fewer than 256 kangaroos, that is
         * small intervals): its points cannot be used. It is kept under another name and the campaign goes on with a new log. */
        char old[640];
        snprintf(old, sizeof old, "%s.v%c", ks->log_path, hdr[4]);
        fclose(f);
        if (rename(ks->log_path, old) != 0) {
            pz_log(PZ_LOG_ERR, "file %s was written by an older layout of the kangaroo and cannot be used, and it could not be moved away", ks->log_path);
            return -1;
        }
        pz_log(PZ_LOG_WARN, "distinguished points: %s was written by an older layout of the kangaroo (versions up to 1.9.0, small intervals) and cannot be "
               "used; it is kept as %s and the campaign goes on with a new file", ks->log_path, old);
        ks->log = kang_log_create(ks, ks->log_path, fp);
        return ks->log ? 1 : -1;
    }
    if (cls == KANG_LOG_NEWER) {
        fclose(f);
        pz_log(PZ_LOG_ERR, "file %s was written by a newer version of pzsolve (%.4s%c) and cannot be used by this one: it is left as it is; use that version, "
               "or move the file away to start the distinguished points again", ks->log_path, (const char *)hdr, isprint(hdr[4]) ? hdr[4] : '?');
        return -1;
    }
    if (cls != KANG_LOG_CURRENT || memcmp(hdr + 6, fp, 32) != 0) {
        fclose(f);
        return log_foreign(ks);
    }
    const long long n = kang_log_read(ks, f);
    const int e = errno;
    fclose(f);
    if (n < 0) return log_unreadable(ks, e);
    /* the whole records of the file: a record cut short at the end (the program stopped in the middle of a write) is cut off, and only that */
    const u64 rest = (u64)size - sizeof hdr, whole = rest / sizeof(dp_rec);
    if (rest % sizeof(dp_rec) && truncate(ks->log_path, (off_t)(sizeof hdr + whole * sizeof(dp_rec))) != 0) {
        pz_log(PZ_LOG_ERR, "cannot cut the partly written record off the end of %s: %s", ks->log_path, strerror(errno));
        return -1;
    }
    ks->loaded = (u64)n;
    ks->log_recs = whole;
    ks->log = fopen(ks->log_path, "ab");
    return ks->log ? 0 : -1;
}

/* Adds the distinguished points of another computer's log of the same campaign (same range and public key; the jump table must
 * be the same too, i.e. the same seed, or the walks of the two computers never meet): returns 0, -1 if the file cannot be
 * opened, -2 if it is not a log of this campaign. A collision between a tame and a wild point solves the key. */
int kang_merge_file(kang_state *ks, const char *path, kang_merge_stats *st)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    u8 hdr[38], fp[32];
    kang_log_fp(ks, fp);
    if (fread(hdr, 1, 38, f) != 38 || memcmp(hdr, KANG_LOG_MAGIC, 6) != 0 || memcmp(hdr + 6, fp, 32) != 0) { fclose(f); return -2; }
    dp_rec rec;
    pthread_mutex_lock(&ks->mu);
    while (fread(&rec, sizeof rec, 1, f) == 1) {      /* a partly written last record is ignored */
        st->read++;
        const int rc = kang_table_insert(ks, &rec, false);
        if (rc == 1) st->added++;
        else if (rc == 0) st->dups++;
        else if (rc < 0) st->skipped++;
        else { st->solved = true; break; }
    }
    kang_table_unlock(ks);
    const bool failed = ferror(f);
    fclose(f);
    return failed ? -1 : 0;                           /* a failed read is not the end of the file */
}
