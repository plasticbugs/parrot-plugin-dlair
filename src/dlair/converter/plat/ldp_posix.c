/*
 * ldp_plat.h for Linux and other POSIX systems: the command-line build.
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include "ldp_plat.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#endif

FILE *ldp_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int ldp_fseek64(FILE *f, int64_t ofs)
{
    return fseeko(f, (off_t)ofs, SEEK_SET);
}

int64_t ldp_fsize64(FILE *f)
{
    off_t cur = ftello(f);
    if (fseeko(f, 0, SEEK_END) != 0)
        return -1;
    off_t n = ftello(f);
    fseeko(f, cur, SEEK_SET);
    return (int64_t)n;
}

int ldp_replace(const char *from, const char *to)
{
    return rename(from, to);
}

int ldp_remove(const char *path)
{
    return remove(path);
}

int ldp_mkdir(const char *path)
{
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

FILE *ldp_create_alloc(const char *path, int64_t size, int *alloc)
{
    *alloc = 0;
    if (unlink(path) != 0 && errno != ENOENT)
        return NULL;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return NULL;
#if defined(__APPLE__)
    /* one piece if it can, else the space in any pieces (msdosfs takes the
       first either way); F_PEOFPOSMODE: from the file's end, its start */
    fstore_t st = {F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, (off_t)size, 0};
    if (fcntl(fd, F_PREALLOCATE, &st) != -1)
        *alloc = 1;
    else
    {
        st.fst_flags = F_ALLOCATEALL;
        st.fst_bytesalloc = 0;
        if (fcntl(fd, F_PREALLOCATE, &st) != -1)
            *alloc = 1;
    }
#elif defined(__linux__)
    /* keep the size 0 (FALLOC_FL_KEEP_SIZE): the writes set it; else the
       size too (vfat then writes the zeros now) */
    if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)size) == 0 || fallocate(fd, 0, 0, (off_t)size) == 0)
        *alloc = 1;
#else
    (void)size;
#endif
    FILE *f = fdopen(fd, "r+b");
    if (!f)
        close(fd);
    return f;
}

int64_t ldp_pieces(FILE *f)
{
    if (fflush(f) != 0)
        return 0;
    int fd = fileno(f);
    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0)
        return 0;
    int64_t n = 0;
#if defined(__APPLE__)
    off_t at = 0, last_end = -1;
    while (at < sb.st_size)
    {
        struct log2phys lp;
        memset(&lp, 0, sizeof lp);
        lp.l2p_contigbytes = sb.st_size - at;
        lp.l2p_devoffset = at;
        if (fcntl(fd, F_LOG2PHYS_EXT, &lp) == -1 || lp.l2p_contigbytes <= 0)
            return 0;               /* (msdosfs: ENOTSUP) */
        if (lp.l2p_devoffset != last_end)
            n++;
        last_end = lp.l2p_devoffset + lp.l2p_contigbytes;
        at += lp.l2p_contigbytes;
    }
#elif defined(__linux__)
    enum { EXT = 256 };
    struct fiemap *fm = (struct fiemap *)calloc(1, sizeof *fm + EXT * sizeof(struct fiemap_extent));
    if (!fm)
        return 0;
    uint64_t start = 0, last_end = ~(uint64_t)0;
    for (;;)
    {
        memset(fm, 0, sizeof *fm);
        fm->fm_start = start;
        fm->fm_length = ~(uint64_t)0 - start;
        fm->fm_flags = FIEMAP_FLAG_SYNC;
        fm->fm_extent_count = EXT;
        if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0)
        {
            n = 0;
            break;
        }
        if (!fm->fm_mapped_extents)
            break;
        int done = 0;
        for (uint32_t i = 0; i < fm->fm_mapped_extents; i++)
        {
            const struct fiemap_extent *e = &fm->fm_extents[i];
            if (e->fe_physical != last_end)
                n++;
            last_end = e->fe_physical + e->fe_length;
            start = e->fe_logical + e->fe_length;
            if (e->fe_flags & FIEMAP_EXTENT_LAST)
                done = 1;
        }
        if (done || fm->fm_mapped_extents < EXT)
            break;
    }
    free(fm);
#else
    (void)n;
    return 0;
#endif
    return n;
}

struct ldp_thread
{
    pthread_t t;
    void (*fn)(void *);
    void *arg;
};

static void *trampoline(void *p)
{
    ldp_thread *t = (ldp_thread *)p;
    t->fn(t->arg);
    return NULL;
}

ldp_thread *ldp_thread_start(void (*fn)(void *), void *arg)
{
    ldp_thread *t = (ldp_thread *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->t, NULL, trampoline, t) != 0)
    {
        free(t);
        return NULL;
    }
    return t;
}

void ldp_thread_join(ldp_thread *t)
{
    if (!t)
        return;
    pthread_join(t->t, NULL);
    free(t);
}

struct ldp_monitor
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

ldp_monitor *ldp_monitor_new(void)
{
    ldp_monitor *m = (ldp_monitor *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    pthread_mutex_init(&m->mutex, NULL);
    pthread_cond_init(&m->cond, NULL);
    return m;
}

void ldp_monitor_free(ldp_monitor *m)
{
    if (!m)
        return;
    pthread_cond_destroy(&m->cond);
    pthread_mutex_destroy(&m->mutex);
    free(m);
}

void ldp_lock(ldp_monitor *m)
{
    pthread_mutex_lock(&m->mutex);
}

void ldp_unlock(ldp_monitor *m)
{
    pthread_mutex_unlock(&m->mutex);
}

void ldp_wait(ldp_monitor *m)
{
    pthread_cond_wait(&m->cond, &m->mutex);
}

void ldp_wait_ms(ldp_monitor *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&m->cond, &m->mutex, &ts);
}

void ldp_notify_all(ldp_monitor *m)
{
    pthread_cond_broadcast(&m->cond);
}

int ldp_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

double ldp_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
