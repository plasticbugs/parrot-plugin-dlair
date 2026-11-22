/*
 * ldp_plat.h for Windows: ld2pico.exe. Paths arrive as UTF-8 and are opened
 * through the wide-character API, so any file name works.
 */
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ldp_plat.h"

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <wchar.h>
#include <winioctl.h>

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (!w)
        return NULL;
    if (n <= 0)
        w[0] = 0;
    else
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

FILE *ldp_fopen(const char *path, const char *mode)
{
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

int ldp_fseek64(FILE *f, int64_t ofs)
{
    return _fseeki64(f, ofs, SEEK_SET);
}

int64_t ldp_fsize64(FILE *f)
{
    int64_t cur = _ftelli64(f);
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return -1;
    int64_t n = _ftelli64(f);
    _fseeki64(f, cur, SEEK_SET);
    return n;
}

int ldp_replace(const char *from, const char *to)
{
    wchar_t *wf = widen(from), *wt = widen(to);
    BOOL ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(wf);
    free(wt);
    return ok ? 0 : -1;
}

int ldp_remove(const char *path)
{
    wchar_t *w = widen(path);
    BOOL ok = w && DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int ldp_mkdir(const char *path)
{
    wchar_t *w = widen(path);
    BOOL ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(w);
    return ok ? 0 : -1;
}

FILE *ldp_create_alloc(const char *path, int64_t size, int *alloc)
{
    *alloc = 0;
    wchar_t *w = widen(path);
    if (!w)
        return NULL;
    if (!DeleteFileW(w) && GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        free(w);
        return NULL;
    }
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    /* the whole allocation at once: FAT's driver looks for a run of free
       clusters big enough; ldp_pieces then says how it went */
    FILE_ALLOCATION_INFO ai;
    ai.AllocationSize.QuadPart = size;
    if (SetFileInformationByHandle(h, FileAllocationInfo, &ai, sizeof ai))
        *alloc = 1;
    int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd < 0)
    {
        CloseHandle(h);
        return NULL;
    }
    FILE *f = _fdopen(fd, "r+b");
    if (!f)
        _close(fd);
    return f;
}

int64_t ldp_pieces(FILE *f)
{
    if (fflush(f) != 0)
        return 0;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    union
    {
        RETRIEVAL_POINTERS_BUFFER rp;
        char bytes[sizeof(RETRIEVAL_POINTERS_BUFFER) + 255 * 2 * sizeof(LARGE_INTEGER)];
    } out;
    STARTING_VCN_INPUT_BUFFER in;
    in.StartingVcn.QuadPart = 0;
    int64_t n = 0, last_end = -1;
    for (;;)
    {
        DWORD got = 0;
        BOOL ok = DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof in, &out, sizeof out, &got, NULL);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err != ERROR_MORE_DATA)
            return 0;
        if (got < sizeof(RETRIEVAL_POINTERS_BUFFER) - sizeof(out.rp.Extents))
            return 0;
        LONGLONG vcn = out.rp.StartingVcn.QuadPart;
        for (DWORD i = 0; i < out.rp.ExtentCount; i++)
        {
            LONGLONG next = out.rp.Extents[i].NextVcn.QuadPart, lcn = out.rp.Extents[i].Lcn.QuadPart;
            if (lcn != -1)
            {
                if (lcn != last_end)
                    n++;
                last_end = lcn + (next - vcn);
            }
            vcn = next;
        }
        if (ok || !out.rp.ExtentCount)
            break;
        in.StartingVcn.QuadPart = vcn;
    }
    return n;
}

struct ldp_thread
{
    HANDLE h;
    void (*fn)(void *);
    void *arg;
};

static DWORD WINAPI trampoline(LPVOID p)
{
    ldp_thread *t = (ldp_thread *)p;
    t->fn(t->arg);
    return 0;
}

ldp_thread *ldp_thread_start(void (*fn)(void *), void *arg)
{
    ldp_thread *t = (ldp_thread *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->h)
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
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

struct ldp_monitor
{
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
};

ldp_monitor *ldp_monitor_new(void)
{
    ldp_monitor *m = (ldp_monitor *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    InitializeCriticalSection(&m->cs);
    InitializeConditionVariable(&m->cv);
    return m;
}

void ldp_monitor_free(ldp_monitor *m)
{
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void ldp_lock(ldp_monitor *m)
{
    EnterCriticalSection(&m->cs);
}

void ldp_unlock(ldp_monitor *m)
{
    LeaveCriticalSection(&m->cs);
}

void ldp_wait(ldp_monitor *m)
{
    SleepConditionVariableCS(&m->cv, &m->cs, INFINITE);
}

void ldp_wait_ms(ldp_monitor *m, int ms)
{
    SleepConditionVariableCS(&m->cv, &m->cs, (DWORD)ms);
}

void ldp_notify_all(ldp_monitor *m)
{
    WakeAllConditionVariable(&m->cv);
}

int ldp_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

double ldp_seconds(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}
