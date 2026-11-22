/*
 * dlair-convert: Dragon's Lair's laserdisc (MAME's dlair.chd) made ready for
 * the Parrot Cart's Dragon's Lair plugin.
 *
 *   dlair-convert dlair.chd <SD card or folder>
 *
 * writes <SD card or folder>/DLAIR/DLAIR.DAT (../dlcard.h): every field's
 * line 17/18 code and sound, and every picture as the console's screen.
 * It checks the CHD is the disc MAME lists before it starts, shows how far
 * it has got, and can be stopped and started again: it carries on where it
 * was.
 *
 * The work is in lib/ (no files there: the CHD comes through a callback):
 * this is the command line, the files and the threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldp_plat.h"
#include "lib/dlc_audio.h"
#include "lib/dlc_card.h"
#include "lib/dlc_chd.h"
#include "lib/dlc_png.h"
#include "lib/dlc_video.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef DLC_VERSION_STR
#define DLC_VERSION_STR "0.1.0"
#endif

/* the disc MAME's dlair set names (dlair.chd's SHA-1) */
static const char *DISC_SHA1 = "c3ee2e27aa4847bf3884fc0d18f26f315456aa9e";

static void usage(void)
{
    fprintf(stderr,
            "dlair-convert " DLC_VERSION_STR ": Dragon's Lair's laserdisc for the Parrot Cart\n"
            "\n"
            "  dlair-convert [options] dlair.chd <SD card or folder>\n"
            "\n"
            "Writes DLAIR/DLAIR.DAT there. dlair.chd is MAME's image of the disc.\n"
            "Stop it at any time: run it again and it carries on.\n"
            "\n"
            "  --look bw|grey|cool|vivid   the picture (default bw: black and white)\n"
            "  --size full|half            half: half as wide and half as tall, in the middle (default full)\n"
            "  --tune K=V,...              change the look: amp chroma bias temp vib sat bright gamma contrast black\n"
            "  --threads N                 how many at once (default: every core)\n"
            "  --info                      show what is on the disc, and stop\n"
            "  --start P --count N         only pictures P to P+N-1 (a trial: the file stays unfinished)\n"
            "  --preview P,P,...           PNGs of these pictures in the folder, no DLAIR.DAT\n"
            "                              (with --look all: in every look)\n");
}

/* --- the CHD through a FILE ------------------------------------------------- */

static int file_read(void *ctx, uint64_t ofs, void *dst, size_t n)
{
    FILE *f = (FILE *)ctx;
    return ldp_fseek64(f, (int64_t)ofs) != 0 || fread(dst, 1, n, f) != n ? -1 : 0;
}

static void sha1_hex(const uint8_t s[20], char out[41])
{
    for (int i = 0; i < 20; i++)
        snprintf(out + 2 * i, 3, "%02x", s[i]);
}

/* --- one picture -------------------------------------------------------------- */

typedef struct
{
    dlc_chd *chd;
    dlc_hunkreader *r;
    uint8_t *woven; /* 704x480x3 */
    uint8_t rgb[DLC_W * DLC_H * 3];
} picwork_t;

/* picture p's fields woven into w->woven; 0 on success, 1 if not on the disc */
static int weave_picture(picwork_t *w, int p)
{
    int nf;
    const int32_t f0 = dlc_chd_picture(w->chd, p, &nf);
    if (f0 < 0)
        return 1;
    for (int k = 0; k < (nf >= 2 ? 2 : 1); k++)
    {
        dlc_field f;
        if (dlc_hunkreader_read(w->r, (uint32_t)f0 + k, 1, &f) || !f.yuy2)
            return -1;
        dlc_weave(f.yuy2, f.width, f.height, (f0 + k) & 1, nf < 2, w->woven);
    }
    return 0;
}

static int write_png(const char *path, const uint8_t *rgb)
{
    size_t n;
    uint8_t *png = dlc_png(rgb, DLC_W, DLC_H, &n);
    FILE *f = png ? ldp_fopen(path, "wb") : NULL;
    int ok = f && fwrite(png, 1, n, f) == n;
    if (f)
        ok &= fclose(f) == 0;
    free(png);
    return ok ? 0 : -1;
}

static int previews(dlc_chd *chd, FILE *cf, const char *list, int look, int half, const char *out)
{
    picwork_t w = {chd, dlc_hunkreader_new(chd, file_read, cf), (uint8_t *)malloc(DLC_SRC_W * DLC_SRC_H * 3), {0}};
    uint8_t *shown = (uint8_t *)malloc(DLC_W * DLC_H * 3), pat[DLC_SCREEN], col[DLC_SCREEN];
    if (!w.r || !w.woven || !shown)
        return 1;
    char path[4096];
    ldp_mkdir(out);
    snprintf(path, sizeof path, "%s/source", out);
    ldp_mkdir(path);
    for (int l = 0; l < DLC_NLOOKS; l++)
        if (look < 0 || l == look)
        {
            snprintf(path, sizeof path, "%s/%s", out, dlc_look_name(l));
            ldp_mkdir(path);
        }
    for (const char *s = list; *s;)
    {
        const int p = atoi(s);
        int got = weave_picture(&w, p);
        if (got)
            fprintf(stderr, "picture %d: %s\n", p, got > 0 ? "not on the disc" : "can't be decoded");
        else
        {
            dlc_fit(w.woven, DLC_BW, w.rgb); /* no grade: the source as the looks start from it */
            snprintf(path, sizeof path, "%s/source/p%05d.png", out, p);
            write_png(path, w.rgb);
            for (int l = 0; l < DLC_NLOOKS; l++)
            {
                if (look >= 0 && l != look)
                    continue;
                if (half)
                {
                    dlc_fit_half(w.woven, l, w.rgb);
                    dlc_encode(w.rgb, l, pat, col, NULL);
                    dlc_blank_outside_half(pat, col);
                    dlc_render(pat, col, shown);
                }
                else
                {
                    dlc_fit(w.woven, l, w.rgb);
                    dlc_encode(w.rgb, l, pat, col, shown);
                }
                snprintf(path, sizeof path, "%s/%s/p%05d.png", out, dlc_look_name(l), p);
                if (write_png(path, shown))
                    fprintf(stderr, "can't write %s\n", path);
                else
                    printf("%s\n", path);
            }
        }
        while (*s && *s != ',')
            s++;
        if (*s == ',')
            s++;
    }
    dlc_hunkreader_free(w.r);
    free(w.woven);
    free(shown);
    return 0;
}

/* --- the conversion ------------------------------------------------------------- */

typedef struct
{
    const char *chd_path;
    dlc_chd *chd;
    const dlc_audio_kernel *kern;
    int look, has_colour;
    int half; /* --size half */
    uint8_t colour;
    dlc_header_t h;
    int32_t *pic_at; /* per hunk: the picture whose first field it is, or -1 */
    uint16_t *chunk_pics; /* pictures a chunk makes */
    uint8_t *want; /* chunks to make this run */
    FILE *out;
    ldp_monitor *m;
    uint32_t next_chunk, made, pics_made, running;
    int failed;
    char err[256];
    dlc_audio_stats stats;
    uint32_t crc_errors;
} job_t;

static int write_at(FILE *f, uint64_t sector, const void *p, size_t n)
{
    return ldp_fseek64(f, (int64_t)(sector * DLC_SECTOR)) != 0 || fwrite(p, 1, n, f) != n ? -1 : 0;
}

static int write_header(job_t *j)
{
    uint8_t sec[DLC_SECTOR];
    dlc_header_pack(&j->h, sec);
    return write_at(j->out, 0, sec, sizeof sec) || fflush(j->out) ? -1 : 0;
}

static void job_fail(job_t *j, const char *msg)
{
    ldp_lock(j->m);
    if (!j->failed)
    {
        j->failed = 1;
        snprintf(j->err, sizeof j->err, "%s", msg);
    }
    ldp_notify_all(j->m);
    ldp_unlock(j->m);
}

typedef struct
{
    int16_t l[DLC_CHD_MAX_FIELD_SAMPLES], r[DLC_CHD_MAX_FIELD_SAMPLES];
    int n;
    int valid;
} ring_t;

typedef struct
{
    job_t *j;
    FILE *cf;
    dlc_hunkreader *r;
    uint8_t *woven;
    uint8_t rgb[DLC_W * DLC_H * 3];
    uint8_t *audio;  /* DLC_CHUNK_HUNKS sectors */
    uint8_t *video;  /* DLC_CHUNK_HUNKS records */
    int32_t *vpic;   /* their picture numbers */
    ring_t ring[3];  /* hunks h-2, h-1, h by h % 3 */
    dlc_audio_stats stats;
} worker_t;

/* makes chunk k into w->audio and w->video; 0 on success */
static int make_chunk(worker_t *w, uint32_t k, int *npics)
{
    job_t *j = w->j;
    const uint32_t hunks = j->h.hunks, lo = k * DLC_CHUNK_HUNKS;
    const uint32_t hi = lo + DLC_CHUNK_HUNKS < hunks ? lo + DLC_CHUNK_HUNKS : hunks;
    const size_t rec = (size_t)j->h.pic_sectors * DLC_SECTOR;
    int pending = -1; /* the picture whose second field comes next */
    *npics = 0;
    memset(w->audio, 0, (size_t)DLC_CHUNK_HUNKS * DLC_SECTOR);
    for (int i = 0; i < 3; i++)
        w->ring[i].valid = 0;
    const uint32_t first = lo ? lo - 1 : 0, last = hi < hunks ? hi : hunks - 1;
    for (uint32_t hh = first; hh <= last; hh++)
    {
        const int starts = hh >= lo && hh < hi && j->pic_at[hh] >= 0;
        int nf = 0;
        if (starts)
            dlc_chd_picture(j->chd, j->pic_at[hh], &nf);
        const int second = pending >= 0;
        dlc_field f;
        if (dlc_hunkreader_read(w->r, hh, starts || second, &f))
        {
            char m[96];
            snprintf(m, sizeof m, "field %u of the disc can't be decoded", hh);
            job_fail(j, m);
            return -1;
        }
        ring_t *rg = &w->ring[hh % 3];
        rg->n = (int)f.samples;
        memcpy(rg->l, f.left, sizeof(int16_t) * f.samples);
        memcpy(rg->r, f.right, sizeof(int16_t) * f.samples);
        rg->valid = 1;
        int done = -1;
        if (second && f.yuy2)
        {
            dlc_weave(f.yuy2, f.width, f.height, hh & 1, 0, w->woven);
            done = pending;
            pending = -1;
        }
        if (starts && f.yuy2)
        {
            dlc_weave(f.yuy2, f.width, f.height, hh & 1, nf < 2, w->woven);
            if (nf < 2)
                done = j->pic_at[hh];
            else
                pending = j->pic_at[hh];
        }
        if (done >= 0)
        {
            uint8_t *v = w->video + (size_t)*npics * rec;
            uint8_t col[DLC_SCREEN];
            uint8_t *const c = j->has_colour ? v + DLC_SCREEN : col;
            if (j->half)
            {
                dlc_fit_half(w->woven, j->look, w->rgb);
                dlc_encode(w->rgb, j->look, v, c, NULL);
                dlc_blank_outside_half(v, c);
            }
            else
            {
                dlc_fit(w->woven, j->look, w->rgb);
                dlc_encode(w->rgb, j->look, v, c, NULL);
            }
            w->vpic[(*npics)++] = done;
        }
        /* the sound of the field before, with this one as its next */
        if (hh >= 1 && hh - 1 >= lo && hh - 1 < hi)
        {
            const uint32_t c = hh - 1;
            dlc_audio_window aw;
            for (int i = 0; i < 3; i++)
            {
                const int64_t src = (int64_t)c - 1 + i;
                const ring_t *q = &w->ring[(uint32_t)(src < 0 ? 0 : src) % 3];
                const int ok = src >= 0 && q->valid;
                aw.l[i] = ok ? q->l : NULL;
                aw.r[i] = ok ? q->r : NULL;
                aw.n[i] = ok ? q->n : 0;
            }
            dlc_audio_field(j->kern, &aw, DLC_AUDIO_GAIN, (int8_t *)(w->audio + (size_t)(c - lo) * DLC_SECTOR), &w->stats);
        }
    }
    if (hi == hunks) /* the disc's last field: no next */
    {
        const uint32_t c = hunks - 1;
        const ring_t *p = c >= 1 && w->ring[(c - 1) % 3].valid ? &w->ring[(c - 1) % 3] : NULL, *q = &w->ring[c % 3];
        dlc_audio_window aw;
        aw.l[0] = p ? p->l : NULL;
        aw.r[0] = p ? p->r : NULL;
        aw.n[0] = p ? p->n : 0;
        aw.l[1] = q->l;
        aw.r[1] = q->r;
        aw.n[1] = q->n;
        aw.l[2] = aw.r[2] = NULL;
        aw.n[2] = 0;
        dlc_audio_field(j->kern, &aw, DLC_AUDIO_GAIN, (int8_t *)(w->audio + (size_t)(c - lo) * DLC_SECTOR), &w->stats);
    }
    return 0;
}

static void worker(void *arg)
{
    worker_t *w = (worker_t *)arg;
    job_t *j = w->j;
    const size_t rec = (size_t)j->h.pic_sectors * DLC_SECTOR;
    for (;;)
    {
        ldp_lock(j->m);
        uint32_t k = j->next_chunk;
        while (k < dlc_chunks(&j->h) && (!j->want[k] || dlc_chunk_done(&j->h, k)))
            k++;
        j->next_chunk = k + 1;
        const int stop = j->failed || k >= dlc_chunks(&j->h);
        ldp_unlock(j->m);
        if (stop)
            break;
        int npics;
        if (make_chunk(w, k, &npics))
            break;
        const uint32_t lo = k * DLC_CHUNK_HUNKS;
        const uint32_t n = lo + DLC_CHUNK_HUNKS < j->h.hunks ? DLC_CHUNK_HUNKS : j->h.hunks - lo;
        ldp_lock(j->m);
        int bad = write_at(j->out, j->h.audio_sector + lo, w->audio, (size_t)n * DLC_SECTOR);
        for (int i = 0; i < npics && !bad; i++)
            bad = write_at(j->out, j->h.video_sector + (uint64_t)(w->vpic[i] - (int32_t)j->h.first_picture) * j->h.pic_sectors,
                           w->video + (size_t)i * rec, rec);
        if (!bad)
            bad = fflush(j->out) != 0;
        if (!bad)
        {
            dlc_chunk_set_done(&j->h, k);
            bad = write_header(j);
        }
        if (bad && !j->failed)
        {
            j->failed = 1;
            snprintf(j->err, sizeof j->err, "can't write DLAIR.DAT (is the card full, or read-only?)");
        }
        j->made++;
        j->pics_made += (uint32_t)npics;
        ldp_notify_all(j->m);
        ldp_unlock(j->m);
        if (bad)
            break;
    }
    ldp_lock(j->m);
    j->running--;
    if (w->stats.peak_l > j->stats.peak_l)
        j->stats.peak_l = w->stats.peak_l;
    if (w->stats.peak_r > j->stats.peak_r)
        j->stats.peak_r = w->stats.peak_r;
    if (w->stats.diff > j->stats.diff)
        j->stats.diff = w->stats.diff;
    j->stats.clipped += w->stats.clipped;
    j->crc_errors += dlc_hunkreader_crc_errors(w->r);
    ldp_notify_all(j->m);
    ldp_unlock(j->m);
}

/* the file's fixed parts: its size, the VBI table, black for pictures not on the disc */
static int write_fixed(job_t *j)
{
    const dlc_header_t *h = &j->h;
    uint8_t zero = 0;
    if (ldp_fseek64(j->out, (int64_t)h->total_sectors * DLC_SECTOR - 1) || fwrite(&zero, 1, 1, j->out) != 1)
        return -1;
    uint8_t *vbi = (uint8_t *)calloc(h->vbi_sectors, DLC_SECTOR);
    if (!vbi)
        return -1;
    for (uint32_t i = 0; i < h->hunks; i++)
    {
        const uint32_t v = dlc_chd_line1718(j->chd, i);
        vbi[4 * i] = (uint8_t)v;
        vbi[4 * i + 1] = (uint8_t)(v >> 8);
        vbi[4 * i + 2] = (uint8_t)(v >> 16);
        vbi[4 * i + 3] = 0;
    }
    int bad = write_at(j->out, h->vbi_sector, vbi, (size_t)h->vbi_sectors * DLC_SECTOR);
    free(vbi);
    const size_t rec = (size_t)h->pic_sectors * DLC_SECTOR;
    uint8_t *black = (uint8_t *)malloc(rec);
    if (!black)
        return -1;
    /* bw: every pixel the set colour (black); else pattern 0 on black and black */
    memset(black, j->has_colour ? 0x00 : 0xFF, DLC_SCREEN);
    if (j->has_colour)
        memset(black + DLC_SCREEN, 0x11, DLC_SCREEN);
    for (uint32_t i = 0; i < h->pictures && !bad; i++)
        if (dlc_chd_picture(j->chd, (int)(h->first_picture + i), NULL) < 0)
            bad = write_at(j->out, h->video_sector + (uint64_t)i * h->pic_sectors, black, rec);
    free(black);
    return bad || fflush(j->out) ? -1 : 0;
}

/* how DLAIR.DAT lies on the card, where the system can say (Windows; not
   macOS on FAT, nor Linux's vfat as far as tried). The plugin reads it in any number of pieces (from 0.1.2:
   dlcardio.c lists its clusters), so this is for interest only. */
static void report_pieces(const char *path, int64_t pieces)
{
    if (pieces == 1)
        printf("%s is in one piece on the disk.\n", path);
    else if (pieces > 1)
        printf("%s is in %lld pieces on the disk.\n", path, (long long)pieces);
}

static void clock_str(double s, char *out, size_t n)
{
    const long t = (long)(s + 0.5);
    if (t >= 3600)
        snprintf(out, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else
        snprintf(out, n, "%ld:%02ld", t / 60, t % 60);
}

static int convert(job_t *j, const char *outdir, int threads, int start, int count)
{
    char path[4096];
    ldp_mkdir(outdir);
    snprintf(path, sizeof path, "%s/DLAIR", outdir);
    if (ldp_mkdir(path))
    {
        fprintf(stderr, "can't make the folder %s\n", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s/DLAIR/DLAIR.DAT", outdir);
    uint8_t sha1[20];
    dlc_chd_sha1(j->chd, sha1);
    const int first = dlc_chd_first(j->chd), last = dlc_chd_last(j->chd);
    dlc_header_layout(&j->h, dlc_chd_hunks(j->chd), (uint32_t)first, (uint32_t)(last - first + 1), j->look, j->has_colour,
                      j->colour, sha1);
    j->h.reserved[0] = j->half ? 1 : 0; /* the size (informational; the plugin plays either), and a resume check */

    /* carry on with the file there, if it is this disc in this look, unfinished */
    int resume = 0;
    j->out = ldp_fopen(path, "r+b");
    if (j->out)
    {
        uint8_t sec[DLC_SECTOR];
        dlc_header_t old;
        if (fread(sec, 1, sizeof sec, j->out) == sizeof sec && !dlc_header_unpack(sec, &old))
        {
            dlc_header_t want = j->h;
            memcpy(want.resume, old.resume, sizeof want.resume);
            want.complete = old.complete;
            uint8_t a[DLC_SECTOR], b[DLC_SECTOR];
            dlc_header_pack(&want, a);
            dlc_header_pack(&old, b);
            if (!memcmp(a, b, sizeof a) && dlc_resume_valid(&old) && ldp_fsize64(j->out) == (int64_t)old.total_sectors * DLC_SECTOR)
            {
                if (old.complete)
                {
                    printf("%s is already made, in the %s look. Delete it to make it again.\n", path, dlc_look_name(j->look));
                    fclose(j->out);
                    return 0;
                }
                j->h = old;
                resume = 1;
            }
        }
        if (!resume)
        {
            fclose(j->out);
            j->out = NULL;
        }
    }
    if (!resume)
    {
        /* made new, never written over in place, its space asked for in
           one go. macOS's msdosfs and Linux's vfat still fill the free
           gaps first (test/contig.sh); the cart reads it in any pieces */
        int alloc;
        j->out = ldp_create_alloc(path, (int64_t)j->h.total_sectors * DLC_SECTOR, &alloc);
        (void)alloc;
        if (!j->out)
        {
            fprintf(stderr, "can't make %s\n", path);
            return 1;
        }
        if (write_header(j) || write_fixed(j))
        {
            fprintf(stderr, "can't write %s (is there room for %.0f MB?)\n", path, j->h.total_sectors / 2048.0);
            return 1;
        }
        report_pieces(path, ldp_pieces(j->out));
        dlc_resume_start(&j->h);
        if (write_header(j))
            return 1;
    }

    const uint32_t chunks = dlc_chunks(&j->h);
    j->want = (uint8_t *)calloc(chunks, 1);
    j->chunk_pics = (uint16_t *)calloc(chunks, sizeof(uint16_t));
    j->pic_at = (int32_t *)malloc(sizeof(int32_t) * j->h.hunks);
    if (!j->want || !j->chunk_pics || !j->pic_at)
        return 1;
    for (uint32_t i = 0; i < j->h.hunks; i++)
        j->pic_at[i] = -1;
    for (int p = first; p <= last; p++)
    {
        const int32_t f0 = dlc_chd_picture(j->chd, p, NULL);
        if (f0 < 0 || (uint32_t)f0 >= j->h.hunks)
            continue;
        j->pic_at[f0] = p;
        j->chunk_pics[f0 / DLC_CHUNK_HUNKS]++;
        if (count <= 0 || (p >= start && p < start + count))
            j->want[f0 / DLC_CHUNK_HUNKS] = 1;
    }
    if (count <= 0)
        memset(j->want, 1, chunks);
    uint32_t todo = 0, pics_todo = 0, pics_all = 0;
    for (uint32_t k = 0; k < chunks; k++)
    {
        pics_all += j->chunk_pics[k];
        if (j->want[k] && !dlc_chunk_done(&j->h, k))
        {
            todo++;
            pics_todo += j->chunk_pics[k];
        }
    }
    char hex[41];
    sha1_hex(sha1, hex);
    printf("%s: %u fields, %u pictures, the %s look%s, %.0f MB\n", path, j->h.hunks, pics_all, dlc_look_name(j->look), j->half ? " at half size" : "",
           j->h.total_sectors / 2048.0);
    if (resume)
        printf("carrying on: %u of %u parts were done, %u to make\n", dlc_chunks_done(&j->h), chunks, todo);

    j->m = ldp_monitor_new();
    if (threads <= 0)
        threads = ldp_cpu_count();
    if (threads > 64)
        threads = 64;
    worker_t *ws = (worker_t *)calloc((size_t)threads, sizeof(worker_t));
    ldp_thread **ts = (ldp_thread **)calloc((size_t)threads, sizeof(ldp_thread *));
    const size_t rec = (size_t)j->h.pic_sectors * DLC_SECTOR;
    for (int i = 0; i < threads; i++)
    {
        worker_t *w = &ws[i];
        w->j = j;
        w->cf = ldp_fopen(j->chd_path, "rb");
        w->r = w->cf ? dlc_hunkreader_new(j->chd, file_read, w->cf) : NULL;
        w->woven = (uint8_t *)malloc(DLC_SRC_W * DLC_SRC_H * 3);
        w->audio = (uint8_t *)malloc((size_t)DLC_CHUNK_HUNKS * DLC_SECTOR);
        w->video = (uint8_t *)malloc(DLC_CHUNK_HUNKS * rec);
        w->vpic = (int32_t *)malloc(sizeof(int32_t) * DLC_CHUNK_HUNKS);
        if (!w->r || !w->woven || !w->audio || !w->video || !w->vpic)
        {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }
    const double t0 = ldp_seconds();
    j->running = (uint32_t)threads;
    for (int i = 0; i < threads; i++)
        ts[i] = ldp_thread_start(worker, &ws[i]);
    ldp_lock(j->m);
    double last_print = 0;
    while (j->running)
    {
        ldp_wait_ms(j->m, 500);
        const double t = ldp_seconds() - t0;
        if (t - last_print < 0.5 && j->running)
            continue;
        last_print = t;
        const double frac = pics_todo ? (double)j->pics_made / pics_todo : 1.0;
        const double rate = t > 0 ? j->pics_made / t : 0;
        char left[32] = "?";
        if (j->pics_made && rate > 0)
            clock_str((pics_todo - j->pics_made) / rate, left, sizeof left);
        fprintf(stderr, "\r  %5.1f%%  %u of %u pictures  %.0f a second  %s left   ", 100.0 * frac, j->pics_made, pics_todo,
                rate, left);
        fflush(stderr);
    }
    ldp_unlock(j->m);
    for (int i = 0; i < threads; i++)
        ldp_thread_join(ts[i]);
    fprintf(stderr, "\n");
    const double secs = ldp_seconds() - t0;
    int rc = 0;
    if (j->failed)
    {
        fprintf(stderr, "stopped: %s\nRun it again to carry on.\n", j->err);
        rc = 1;
    }
    else
    {
        const int all = dlc_chunks_done(&j->h) == chunks;
        if (all)
        {
            j->h.complete = 1;
            if (write_header(j))
                rc = 1;
        }
        char took[32];
        clock_str(secs, took, sizeof took);
        printf("%s %u pictures in %s with %d threads%s\n", all ? "made" : "made a trial of", j->pics_made, took, threads,
               all ? "" : " (the file is unfinished: run without --start/--count to finish it)");
        printf("sound: peaks %.3f left, %.3f right of full scale; left and right differ by %.4f at most; %u samples "
               "clipped\n",
               j->stats.peak_l, j->stats.peak_r, j->stats.diff, j->stats.clipped);
        if (j->crc_errors)
            printf("warning: %u fields failed their CRC check (a damaged CHD?)\n", j->crc_errors);
        if (all)
            printf("done: copy the DLAIR folder to the card, with dlair.zip in it\n");
    }
    if (fclose(j->out))
        rc = 1;
    for (int i = 0; i < threads; i++)
    {
        dlc_hunkreader_free(ws[i].r);
        if (ws[i].cf)
            fclose(ws[i].cf);
        free(ws[i].woven);
        free(ws[i].audio);
        free(ws[i].video);
        free(ws[i].vpic);
    }
    free(ws);
    free(ts);
    ldp_monitor_free(j->m);
    return rc;
}

/* --- main --------------------------------------------------------------------- */

static int run(int argc, char **argv)
{
    const char *pos[2] = {NULL, NULL}, *preview = NULL, *look_name = "bw", *size_name = "full", *tune = NULL;
    int npos = 0, threads = 0, start = 0, count = 0, info = 0;
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const int more = i + 1 < argc;
        if (!strcmp(a, "--look") && more)
            look_name = argv[++i];
        else if (!strcmp(a, "--size") && more)
            size_name = argv[++i];
        else if (!strcmp(a, "--tune") && more)
            tune = argv[++i];
        else if (!strcmp(a, "--threads") && more)
            threads = atoi(argv[++i]);
        else if (!strcmp(a, "--start") && more)
            start = atoi(argv[++i]);
        else if (!strcmp(a, "--count") && more)
            count = atoi(argv[++i]);
        else if (!strcmp(a, "--preview") && more)
            preview = argv[++i];
        else if (!strcmp(a, "--info"))
            info = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
        {
            usage();
            return 0;
        }
        else if (a[0] == '-' && a[1] == '-')
        {
            fprintf(stderr, "unknown option %s\n\n", a);
            usage();
            return 2;
        }
        else if (npos < 2)
            pos[npos++] = a;
        else
        {
            usage();
            return 2;
        }
    }
    if (npos < (info ? 1 : 2))
    {
        usage();
        return 2;
    }
    const int look = !strcmp(look_name, "all") && preview ? -1 : dlc_look_id(look_name);
    if (look < 0 && !(preview && !strcmp(look_name, "all")))
    {
        fprintf(stderr, "no look named %s (bw, grey, cool or vivid)\n", look_name);
        return 2;
    }
    if (strcmp(size_name, "full") && strcmp(size_name, "half"))
    {
        fprintf(stderr, "no size named %s (full or half)\n", size_name);
        return 2;
    }
    const int half = !strcmp(size_name, "half");
    if (tune && (look < 0 || dlc_tune(look, tune)))
    {
        fprintf(stderr, "can't use --tune %s (one look, and keys amp chroma bias temp vib sat bright gamma contrast black)\n", tune);
        return 2;
    }

    FILE *cf = ldp_fopen(pos[0], "rb");
    if (!cf)
    {
        fprintf(stderr, "can't open %s\n", pos[0]);
        return 1;
    }
    char err[256];
    dlc_chd *chd = dlc_chd_open(file_read, cf, err, sizeof err);
    if (!chd)
    {
        fprintf(stderr, "%s: %s\n", pos[0], err);
        return 1;
    }
    uint8_t sha1[20];
    char hex[41];
    dlc_chd_sha1(chd, sha1);
    sha1_hex(sha1, hex);
    if (info)
    {
        char s[512];
        dlc_chd_info(chd, s, sizeof s);
        printf("%s\nSHA-1 %s (%s)\n", s, hex, strcmp(hex, DISC_SHA1) ? "not the dlair disc" : "the dlair disc");
        return 0;
    }
    if (strcmp(hex, DISC_SHA1))
    {
        fprintf(stderr,
                "%s is not the Dragon's Lair disc this needs.\n"
                "Its SHA-1 is %s; MAME's dlair set lists %s.\n",
                pos[0], hex, DISC_SHA1);
        return 1;
    }
    int rc;
    if (preview)
        rc = previews(chd, cf, preview, look, half, pos[1]);
    else
    {
        job_t j;
        memset(&j, 0, sizeof j);
        j.chd_path = pos[0];
        j.chd = chd;
        j.look = look;
        j.half = half;
        j.has_colour = dlc_look_has_colour(look, &j.colour);
        dlc_audio_kernel *k = dlc_audio_kernel_new();
        j.kern = k;
        rc = k ? convert(&j, pos[1], threads, start, count) : 1;
        dlc_audio_kernel_free(k);
        free(j.pic_at);
        free(j.chunk_pics);
        free(j.want);
    }
    dlc_chd_close(chd);
    fclose(cf);
    return rc;
}

#ifdef _WIN32
/* the arguments as UTF-8 (what ldp_fopen takes), whatever the code page */
int main(void)
{
    int argc;
    wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char *)malloc((size_t)(n > 0 ? n : 1));
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, argv[i], n, NULL, NULL);
    }
    LocalFree(wv);
    return run(argc, argv);
}
#else
int main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif
