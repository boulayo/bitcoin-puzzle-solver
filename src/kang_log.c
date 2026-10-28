/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_log.c - the file of distinguished points of a kangaroo campaign (dp.bin): creation, loading, appending with recovery from
 * write errors, and the merge of another computer's file. The table it feeds is in kangaroo.c. */
#include "kangaroo_priv.h"
#include <pzcore/hash.h>
#include "platform.h"
#include <unistd.h>

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

/* A write failed (disk full, quota, I/O error): what reached the file is unknown, maybe a torn record, maybe whole ones. Reopens the file,
 * cuts it back to whole records and drops from the buffer the records that did get there, so the next write appends exactly the rest. */
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
    const u64 on_file = whole > ks->log_recs ? whole - ks->log_recs : 0;      /* of the buffered records, already written */
    const size_t drop = on_file > ks->nlog ? ks->nlog : (size_t)on_file;
    memmove(ks->logbuf, ks->logbuf + drop, (ks->nlog - drop) * sizeof(dp_rec));
    ks->nlog -= drop;
    ks->log_recs = whole;
    ks->log = fopen(ks->log_path, "ab");
    if (!ks->log) return -1;
    ks->log_broken = false;
    return 0;
}

int kang_flush_log(kang_state *ks)
{
    if (!ks->log && !ks->log_broken) return 0;                       /* no log at all (test states) */
    if (ks->log_broken && log_resync(ks) != 0) return -1;
    if (ks->nlog && (fwrite(ks->logbuf, sizeof(dp_rec), ks->nlog, ks->log) != ks->nlog || fflush(ks->log) != 0)) {
        ks->log_broken = true;
        return -1;
    }
    if (ks->nlog) {
        ks->log_recs += ks->nlog;
        ks->nlog = 0;
        ks->log_warned = false;
        return 0;
    }
    if (fflush(ks->log) != 0) { ks->log_broken = true; return -1; }
    return 0;
}

/* mu held. Appends to the log buffer, writing it out when it is full. When the disk refuses, the buffer grows (up to KANG_LOG_BUF_MAX
 * records) instead of overflowing, and the next checkpoint tries again; beyond that new points stay in the table only. */
void kang_log_push(kang_state *ks, const dp_rec *rec)
{
    if (!ks->log && !ks->log_broken) return;
    if (ks->nlog == ks->caplog) {
        kang_flush_log(ks);
        if (ks->nlog == ks->caplog) {
            if (ks->caplog >= KANG_LOG_BUF_MAX) {
                atomic_fetch_add(&ks->dropped, 1);
                if (!ks->log_warned) {
                    ks->log_warned = true;
                    pz_log(PZ_LOG_ERR, "writing %s failed (disk full?): new distinguished points stay in RAM only and will be lost if the program stops", ks->log_path);
                }
                return;
            }
            ks->caplog *= 2;
            ks->logbuf = pz_xrealloc(ks->logbuf, ks->caplog * sizeof(dp_rec));
        }
    }
    ks->logbuf[ks->nlog++] = *rec;
}

/* Fingerprint of the logs of this campaign (see KANG_LOG_MAGIC). */
void kang_log_fp(const kang_state *ks, u8 fp[32]) { log_fingerprint(ks, &ks->base, "PZDPLOG4", fp); }

FILE *kang_log_create(const kang_state *ks, const char *path, const u8 fp[32])
{
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite(KANG_LOG_MAGIC, 1, 6, f);
    fwrite(fp, 1, 32, f);
    fflush(f);
    return f;
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
    u8 hdr[38];
    bool hdr_ok = fread(hdr, 1, 38, f) == 38;
    if (hdr_ok && !memcmp(hdr, "PZDP", 4) && memcmp(hdr, KANG_LOG_MAGIC, 6) != 0) {
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
    if (!hdr_ok || memcmp(hdr, KANG_LOG_MAGIC, 6) != 0 || memcmp(hdr + 6, fp, 32) != 0) {
        fclose(f);
        pz_log(PZ_LOG_ERR, "file %s does not belong to this campaign (different header): ignored", ks->log_path);
        return -1;
    }
    dp_rec rec;
    u64 n = 0;
    pthread_mutex_lock(&ks->mu);
    while (fread(&rec, sizeof rec, 1, f) == 1) {
        int rc = kang_table_insert(ks, &rec, true);
        n++;
        if (rc == 2) break;
    }
    pthread_mutex_unlock(&ks->mu);
    fclose(f);
    ks->loaded = n;
    ks->log_recs = n;
    /* truncate any partially written tail record and continue appending */
    off_t good = 38 + (off_t)(n * sizeof(dp_rec));
    if (truncate(ks->log_path, good) != 0) { /* best effort */ }
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
    pthread_mutex_unlock(&ks->mu);
    fclose(f);
    return 0;
}
