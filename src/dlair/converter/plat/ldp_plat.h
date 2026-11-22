/*
 * What the ld2pico core needs from the operating system: files larger than
 * 4 GB (the CHD is 12 GB), threads, and a clock. All paths are UTF-8.
 * ldp_win32.c implements this for Windows, ldp_posix.c for macOS and Linux.
 * (From pico-dragonslair's tools/ld2pico, GPL-3.0-or-later; ldp_mkdir added.)
 */
#ifndef LDP_PLAT_H
#define LDP_PLAT_H

#include <stdint.h>
#include <stdio.h>

FILE *ldp_fopen(const char *path, const char *mode);
int ldp_fseek64(FILE *f, int64_t ofs); /* 0 on success */
int64_t ldp_fsize64(FILE *f);
/* Renames over an existing file; 0 on success. */
int ldp_replace(const char *from, const char *to);
int ldp_remove(const char *path);
/* Makes a folder; 0 on success or if it is there already. */
int ldp_mkdir(const char *path);

/* A big file made new: path deleted if it is there (never written over in
 * place), then created and its whole size asked for in one request, so the
 * file system can put it in one run of free space if it has one (macOS
 * F_PREALLOCATE, F_ALLOCATECONTIG asked; Windows' allocation size; Linux's
 * fallocate). None of them promise one piece on FAT: macOS's msdosfs and
 * Linux's vfat fill the free gaps from the start whatever was asked
 * (test/contig.sh). Returns it open for reading and writing
 * (its length still 0), or NULL. *alloc: 1 the space was taken ahead, 0 it
 * wasn't (the writes take it as they go). */
FILE *ldp_create_alloc(const char *path, int64_t size, int *alloc);
/* How many pieces an open file is in on its disk (runs of clusters that
 * follow each other count as one), or 0 if the system can't say (macOS's
 * FAT driver can't). */
int64_t ldp_pieces(FILE *f);

typedef struct ldp_thread ldp_thread;
ldp_thread *ldp_thread_start(void (*fn)(void *), void *arg);
void ldp_thread_join(ldp_thread *t);

/* A mutex with one condition variable. */
typedef struct ldp_monitor ldp_monitor;
ldp_monitor *ldp_monitor_new(void);
void ldp_monitor_free(ldp_monitor *m);
void ldp_lock(ldp_monitor *m);
void ldp_unlock(ldp_monitor *m);
void ldp_wait(ldp_monitor *m); /* with the lock held */
void ldp_wait_ms(ldp_monitor *m, int ms);
void ldp_notify_all(ldp_monitor *m);

int ldp_cpu_count(void);
double ldp_seconds(void); /* monotonic */

#endif
