/* JSRF launcher: files, the log, running programs (Windows and POSIX). */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ======================================================================
 * paths
 * ==================================================================== */
void pjoin(char *out, const char *a, const char *b)
{
    char tmp[LPATH];
    size_t n = strlen(a);
    if (n && (a[n - 1] == '/' || a[n - 1] == '\\'))
        snprintf(tmp, sizeof tmp, "%s%s", a, b);
    else
        snprintf(tmp, sizeof tmp, "%s" SEP "%s", a, b);
#ifdef _WIN32
    for (char *c = tmp; *c; c++) if (*c == '/') *c = '\\';
#endif
    memcpy(out, tmp, strlen(tmp) + 1);
}

void pjoin3(char *out, const char *a, const char *b, const char *c)
{
    char t[LPATH];
    pjoin(t, a, b);
    pjoin(out, t, c);
}

#ifdef _WIN32
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}
static void narrow(const wchar_t *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}
#endif

void exe_dir(char *out)
{
    char *b = SDL_GetBasePath();   /* with a trailing separator */
    if (b) {
        snprintf(out, LPATH, "%s", b);
        SDL_free(b);
        size_t n = strlen(out);
        while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\')) out[--n] = 0;
    } else {
        snprintf(out, LPATH, ".");
    }
}

bool is_file(const char *p)
{
#ifdef _WIN32
    wchar_t *w = wide(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool is_dir(const char *p)
{
#ifdef _WIN32
    wchar_t *w = wide(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool mkdir1(const char *p)
{
#ifdef _WIN32
    wchar_t *w = wide(p);
    BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}

bool make_dirs(const char *p)
{
    char t[LPATH];
    snprintf(t, sizeof t, "%s", p);
    for (char *c = t + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
            if (!(strlen(t) == 2 && t[1] == ':')) mkdir1(t);
            *c = k;
        }
    }
    return mkdir1(t) && is_dir(t);
}

bool copy_file(const char *src, const char *dst)
{
#ifdef _WIN32
    wchar_t *a = wide(src), *b = wide(dst);
    BOOL ok = CopyFileW(a, b, FALSE);
    free(a); free(b);
    return ok;
#else
    size_t n;
    char *d = read_file(src, &n);
    if (!d) return false;
    struct stat st;
    bool ok = write_file(dst, d, n);
    free(d);
    if (ok && stat(src, &st) == 0) chmod(dst, st.st_mode & 0777);
    return ok;
#endif
}

bool remove_file(const char *p)
{
#ifdef _WIN32
    wchar_t *w = wide(p);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok;
#else
    return unlink(p) == 0;
#endif
}

static void rm_cb(const char *name, bool isdir, void *u)
{
    char p[LPATH];
    pjoin(p, (const char *)u, name);
    if (isdir) remove_tree(p); else remove_file(p);
}

bool remove_tree(const char *p)
{
    if (!is_dir(p)) return !is_file(p) || remove_file(p);
    list_dir(p, rm_cb, (void *)p);
#ifdef _WIN32
    wchar_t *w = wide(p);
    BOOL ok = RemoveDirectoryW(w);
    free(w);
    return ok;
#else
    return rmdir(p) == 0;
#endif
}

bool move_path(const char *src, const char *dst)
{
#ifdef _WIN32
    wchar_t *a = wide(src), *b = wide(dst);
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a); free(b);
    return ok;
#else
    return rename(src, dst) == 0;
#endif
}

static FILE *uopen(const char *p, const char *mode)
{
#ifdef _WIN32
    wchar_t *w = wide(p), *m = wide(mode);
    FILE *f = _wfopen(w, m);
    free(w); free(m);
    return f;
#else
    return fopen(p, mode);
#endif
}

char *read_file(const char *p, size_t *len)
{
    FILE *f = uopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *d = malloc((size_t)n + 1);
    size_t got = fread(d, 1, (size_t)n, f);
    fclose(f);
    d[got] = 0;
    if (len) *len = got;
    return d;
}

bool write_file(const char *p, const void *data, size_t len)
{
    FILE *f = uopen(p, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

void list_dir(const char *dir, void (*cb)(const char *, bool, void *), void *u)
{
#ifdef _WIN32
    char pat[LPATH];
    pjoin(pat, dir, "*");
    wchar_t *w = wide(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[LPATH];
        narrow(fd.cFileName, name, sizeof name);
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        cb(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, u);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    /* names first: the callback may delete entries */
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, sizeof(char *) * cap); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[LPATH];
        pjoin(p, dir, names[i]);
        struct stat st;
        bool isdir = lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
        cb(names[i], isdir, u);
        free(names[i]);
    }
    free(names);
#endif
}

void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    wchar_t *a = wide(k), *b = v ? wide(v) : NULL;
    SetEnvironmentVariableW(a, b);
    free(a); free(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

int cpu_count(void)
{
    int n = SDL_GetCPUCount();
    return n > 0 ? n : 2;
}

/* ======================================================================
 * fingerprints (FNV-1a 64)
 * ==================================================================== */
static unsigned long long fnv(const void *d, size_t n, unsigned long long h)
{
    const unsigned char *p = d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

unsigned long long hash_text(const char *s, unsigned long long seed)
{
    return fnv(s, strlen(s) + 1, seed);
}

typedef struct { char **v; int n, cap; const char *root; const char *const *skip; } FileList;

static void fl_add(FileList *l, const char *rel)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = realloc(l->v, sizeof(char *) * l->cap); }
    l->v[l->n++] = strdup(rel);
}

static bool fp_skip(const char *name)
{
    size_t n = strlen(name);
    return !strcmp(name, "__pycache__") || !strcmp(name, ".git") ||
           (n > 4 && !strcmp(name + n - 4, ".pyc")) || !strncmp(name, "launcher_", 9) ||
           (!strncmp(name, "test_", 5) && n > 3 && !strcmp(name + n - 3, ".py"));   /* tests */
}

typedef struct { FileList *l; const char *rel; } FlWalk;
static void fl_walk(FileList *l, const char *rel);

static void fl_cb(const char *name, bool isdir, void *u)
{
    FlWalk *w = u;
    if (fp_skip(name)) return;
    char r[LPATH];
    snprintf(r, sizeof r, "%s/%s", w->rel, name);   /* "/" on every system: same value */
    for (int i = 0; w->l->skip && w->l->skip[i]; i++)
        if (!strcmp(r, w->l->skip[i])) return;
    if (isdir) fl_walk(w->l, r); else fl_add(w->l, r);
}

static void fl_walk(FileList *l, const char *rel)
{
    char p[LPATH];
    pjoin(p, l->root, rel);
    FlWalk w = { l, rel };
    list_dir(p, fl_cb, &w);
}

static int fl_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

unsigned long long fingerprint(const char *root, const char *const *items,
                               const char *const *skip, unsigned long long h)
{
    FileList l = { NULL, 0, 0, root, skip };
    for (int i = 0; items[i]; i++) {
        char p[LPATH];
        pjoin(p, root, items[i]);
        if (is_dir(p)) fl_walk(&l, items[i]);
        else if (is_file(p)) fl_add(&l, items[i]);
        else h = hash_text(items[i], h ^ 0x6d697373ULL);   /* "miss" */
    }
    qsort(l.v, (size_t)l.n, sizeof(char *), fl_cmp);
    for (int i = 0; i < l.n; i++) {
        char p[LPATH];
        pjoin(p, root, l.v[i]);
        size_t n = 0;
        char *d = read_file(p, &n);
        h = hash_text(l.v[i], h);
        if (d) { h = fnv(&n, sizeof n, h); h = fnv(d, n, h); free(d); }
        free(l.v[i]);
    }
    free(l.v);
    return h;
}

/* ======================================================================
 * the log
 * ==================================================================== */
#define LOG_MAX 4000
static SDL_mutex *s_log_mx;
static char *s_lines[LOG_MAX];
static int s_nlines;
static bool s_open;          /* last line still being written */
static bool s_cr;            /* a \r came: the next text replaces the line */
static unsigned s_serial;

static FILE *s_log_file;

void log_init(void) { s_log_mx = SDL_CreateMutex(); }

void log_file_open(const char *path)
{
    char old[LPATH];
    snprintf(old, sizeof old, "%s.old", path);
    remove_file(old);
    move_path(path, old);
    s_log_file = uopen(path, "w");
    if (s_log_file) {
        char t[64];
        time_t now = time(NULL);
        strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(s_log_file, "JSRF launcher log, %s\n", t);
        fflush(s_log_file);
    }
}

static void push_line(void)
{
    if (s_nlines == LOG_MAX) {
        free(s_lines[0]);
        memmove(s_lines, s_lines + 1, sizeof(char *) * (LOG_MAX - 1));
        s_nlines--;
    }
    s_lines[s_nlines++] = strdup("");
    s_open = true;
}

static void append(const char *s, size_t n)
{
    if (!s_open) push_line();
    char **l = &s_lines[s_nlines - 1];
    if (s_cr) { (*l)[0] = 0; s_cr = false; }
    size_t m = strlen(*l);
    if (m > 2000) return;
    *l = realloc(*l, m + n + 1);
    memcpy(*l + m, s, n);
    (*l)[m + n] = 0;
}

/* v2i: how far the running step is, from its output: ninja's "[n/m]",
 * generate_code.py's "[k/4]" steps and the translator's own lines. -1: not
 * known (the bar then just moves). */
static float s_prog = -1.0f;
static int s_gen_stage;

void log_progress_reset(void)
{
    if (s_log_mx) SDL_LockMutex(s_log_mx);
    s_prog = -1.0f;
    s_gen_stage = 0;
    if (s_log_mx) SDL_UnlockMutex(s_log_mx);
}

float log_progress(void) { return s_prog; }

static void prog_line(const char *l)
{
    int a, b;
    while (*l == ' ') l++;
    if (sscanf(l, "[%d/%d]", &a, &b) == 2 && b > 0 && a >= 0 && a <= b) {
        if (b == 4 && !strstr(l, "Building") && !strstr(l, "Linking")) {
            /* generate_code.py: [1/4] layout, [2/4] translating, [3/4] the
             * functions lifted on their own, [4/4] fix-ups */
            static const float at[5] = { 0, 0.02f, 0.05f, 0.62f, 0.94f };
            s_gen_stage = a;
            s_prog = at[a];
        } else {
            s_prog = (float)a / (float)b;
        }
        return;
    }
    {
        /* curl -#: "######    45.3%" */
        size_t n = strlen(l);
        if (n > 1 && l[n - 1] == '%') {
            const char *q = l + n - 1;
            while (q > l && (q[-1] == '.' || (q[-1] >= '0' && q[-1] <= '9'))) q--;
            if (q < l + n - 1) {
                float v = (float)atof(q);
                if (v >= 0 && v <= 100) { s_prog = v / 100.0f; return; }
            }
        }
    }
    if (!s_gen_stage) return;
    if (s_gen_stage == 2 && strstr(l, "Translating ") && strstr(l, "functions")) s_prog = 0.30f;
    else if (s_gen_stage == 2 && strstr(l, "Split Translation Complete")) s_prog = 0.58f;
    else if (s_gen_stage == 3 && strstr(l, "Loading data files")) { s_prog += 0.07f; if (s_prog > 0.92f) s_prog = 0.92f; }
    else if (strncmp(l, "Done:", 5) == 0) s_prog = 1.0f;
}

void log_text(const char *s, size_t n)
{
    SDL_LockMutex(s_log_mx);
    size_t a = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            if (i > a) append(s + a, i - a);
            if (s_open) prog_line(s_lines[s_nlines - 1]);
            if (s[i] == '\n') {
                if (!s_open) push_line();
                if (s_log_file) {
                    fprintf(s_log_file, "%s\n", s_lines[s_nlines - 1]);
                    fflush(s_log_file);
                }
                s_open = false;
                s_cr = false;
            } else if (s_open) {
                s_cr = true;
            }
            a = i + 1;
        }
    }
    if (n > a) append(s + a, n - a);
    s_serial++;
    SDL_UnlockMutex(s_log_mx);
}

void logf_(const char *fmt, ...)
{
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 3) n = sizeof b - 3;
    SDL_LockMutex(s_log_mx);
    if (s_open) { s_open = false; s_cr = false; }
    SDL_UnlockMutex(s_log_mx);
    b[n++] = '\n';
    log_text(b, (size_t)n);
}

void log_clear(void)
{
    SDL_LockMutex(s_log_mx);
    for (int i = 0; i < s_nlines; i++) free(s_lines[i]);
    s_nlines = 0;
    s_open = s_cr = false;
    s_serial++;
    SDL_UnlockMutex(s_log_mx);
}

int log_lock(const char ***lines)
{
    SDL_LockMutex(s_log_mx);
    *lines = (const char **)s_lines;
    return s_nlines;
}

void log_unlock(void) { SDL_UnlockMutex(s_log_mx); }

unsigned log_serial(void) { return s_serial; }

/* ======================================================================
 * programs
 * ==================================================================== */
/* run_capture: the output goes to a buffer instead of the log */
static char *s_cap;
static size_t s_cap_n, s_cap_len;

static void out_text(const char *s, size_t n)
{
    if (!s_cap) { log_text(s, n); return; }
    if (s_cap_len + n >= s_cap_n) n = s_cap_n - 1 - s_cap_len;
    memcpy(s_cap + s_cap_len, s, n);
    s_cap_len += n;
    s_cap[s_cap_len] = 0;
}
#ifdef _WIN32
/* one argument, quoted the way the C runtime reads it back */
static void quote_arg(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *a)
{
#define PUT(ch) do { if (*len + 2 >= *cap) { *cap = *cap * 2 + 64; *buf = realloc(*buf, *cap * sizeof(wchar_t)); } (*buf)[(*len)++] = (ch); } while (0)
    if (*len) PUT(L' ');
    bool q = !*a || wcspbrk(a, L" \t\"") != NULL;
    if (q) PUT(L'"');
    for (const wchar_t *c = a;; c++) {
        size_t bs = 0;
        while (*c == L'\\') { bs++; c++; }
        if (!*c) {
            for (size_t i = 0; i < (q ? bs * 2 : bs); i++) PUT(L'\\');
            break;
        }
        if (*c == L'"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\');
            PUT(L'"');
        } else {
            for (size_t i = 0; i < bs; i++) PUT(L'\\');
            PUT(*c);
        }
    }
    if (q) PUT(L'"');
    (*buf)[*len] = 0;
#undef PUT
}

static wchar_t *command_line(const char *const *argv)
{
    size_t len = 0, cap = 256;
    wchar_t *buf = malloc(cap * sizeof(wchar_t));
    buf[0] = 0;
    for (int i = 0; argv[i]; i++) {
        wchar_t *w = wide(argv[i]);
        quote_arg(&buf, &len, &cap, w);
        free(w);
    }
    return buf;
}

int run(const char *const *argv, const char *cwd, volatile int *cancel)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    wchar_t *cl = command_line(argv);
    wchar_t *wd = cwd ? wide(cwd) : NULL;
    /* a job, so cancelling also stops what the program started (compilers) */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
    memset(&jl, 0, sizeof jl);
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof jl);
    BOOL ok = CreateProcessW(NULL, cl, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, wd, &si, &pi);
    free(cl);
    free(wd);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        logf_("Could not start %s (error %lu).", argv[0], GetLastError());
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    char b[4096];
    bool cancelled = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;   /* closed */
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(rd, b, avail < sizeof b ? avail : sizeof b, &got, NULL) || !got) break;
            out_text(b, got);
            continue;
        }
        if (cancel && *cancel && !cancelled) {
            cancelled = true;
            if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            /* drain what is left */
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
                DWORD got = 0;
                if (!ReadFile(rd, b, avail < sizeof b ? avail : sizeof b, &got, NULL) || !got) break;
                out_text(b, got);
            }
            break;
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (job) CloseHandle(job);
    return cancelled ? -2 : (int)code;
}

bool launch(const char *exe, const char *cwd)
{
    const char *argv[] = { exe, NULL };
    wchar_t *cl = command_line(argv);
    wchar_t *wd = wide(cwd);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cl, NULL, NULL, FALSE, 0, NULL, wd, &si, &pi);
    free(cl);
    free(wd);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok;
}

bool pick_folder(const char *start, char *out)
{
    IFileOpenDialog *d = NULL;
    IShellItem *it = NULL;
    PWSTR p = NULL;
    bool ok = false;
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&d))) {
        if (SUCCEEDED(ci)) CoUninitialize();
        return false;
    }
    FILEOPENDIALOGOPTIONS o = 0;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(d, L"Choose the game folder (with default.xbe)");
    if (start && *start && is_dir(start)) {
        wchar_t *w = wide(start);
        IShellItem *sf = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(w, NULL, &IID_IShellItem, (void **)&sf))) {
            IFileOpenDialog_SetFolder(d, sf);
            IShellItem_Release(sf);
        }
        free(w);
    }
    if (SUCCEEDED(IFileOpenDialog_Show(d, GetActiveWindow())) &&
        SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
        if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            narrow(p, out, LPATH);
            ok = out[0] != 0;
            CoTaskMemFree(p);
        }
        IShellItem_Release(it);
    }
    IFileOpenDialog_Release(d);
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

void open_folder(const char *p)
{
    wchar_t *w = wide(p);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

#else /* POSIX */

int run(const char *const *argv, const char *cwd, volatile int *cancel)
{
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        /* the job runs on an SDL thread, which has every signal blocked: a
         * program started with SIGCHLD blocked never sees its own children
         * end (CMake hangs) */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setpgid(0, 0);   /* its own group: cancel stops the compilers too */
        /* Steam's game mode preloads its overlay into everything started
         * from it; the 32-bit copy makes every 64-bit tool print an ld.so
         * error, which ended up in the versions read back from git and curl.
         * The tools don't need the overlay (the game keeps it). */
        unsetenv("LD_PRELOAD");
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        if (cwd && chdir(cwd) != 0) _exit(126);
        execvp(argv[0], (char *const *)argv);
        dprintf(2, "Could not start %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(fd[1]);
    char b[4096];
    bool cancelled = false;
    for (;;) {
        struct pollfd p = { fd[0], POLLIN, 0 };
        int r = poll(&p, 1, 50);
        if (r > 0) {
            ssize_t n = read(fd[0], b, sizeof b);
            if (n <= 0) break;
            out_text(b, (size_t)n);
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        if (cancel && *cancel && !cancelled) {
            cancelled = true;
            kill(-pid, SIGTERM);
        }
    }
    close(fd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (cancelled) return -2;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

bool launch(const char *exe, const char *cwd)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        /* a grandchild, so the launcher never has to wait for the game
         * (same session: Steam still sees the game as part of the app) */
        if (fork() != 0) _exit(0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        if (chdir(cwd) != 0) _exit(126);
        execl(exe, exe, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return true;
}

/* the desktop's own dialog: kdialog (KDE, the Steam Deck's desktop), else
 * zenity (GNOME and others) */
bool pick_folder(const char *start, char *out)
{
    const char *dir = start && *start && is_dir(start) ? start : getenv("HOME");
    char title[] = "Choose the game folder (with default.xbe)";
    const char *kd[] = { "kdialog", "--title", title, "--getexistingdirectory", dir ? dir : "/", NULL };
    char d2[LPATH + 2];
    snprintf(d2, sizeof d2, "%s/", dir ? dir : "");
    const char *ze[] = { "zenity", "--file-selection", "--directory", "--title", title, "--filename", d2, NULL };
    const char *const *tries[] = { kd, ze };
    for (int i = 0; i < 2; i++) {
        int r = run_capture(tries[i], NULL, out, LPATH);
        if (r == 0 && out[0] == '/') return true;
        if (r == 1 || (r == 0 && !out[0])) return false;   /* cancelled */
    }
    out[0] = 0;
    return false;
}

void open_folder(const char *p)
{
    char url[LPATH + 8];
    snprintf(url, sizeof url, "file://%s", p);
    SDL_OpenURL(url);
}
#endif

int run_capture(const char *const *argv, const char *cwd, char *out, size_t cap)
{
    out[0] = 0;
    s_cap = out;
    s_cap_n = cap;
    s_cap_len = 0;
    int r = run(argv, cwd, NULL);
    s_cap = NULL;
    /* no line ending at the end */
    while (s_cap_len && (out[s_cap_len - 1] == '\n' || out[s_cap_len - 1] == '\r' || out[s_cap_len - 1] == ' '))
        out[--s_cap_len] = 0;
    return r;
}

/* ---- SHA-256 (FIPS 180-4), for comparing the generated code with the
 * project's gen_hashes.txt ---- */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void sha256_hex(const unsigned char *d, size_t n, char out[65])
{
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    unsigned char last[128] = { 0 };
    size_t full = n & ~(size_t)63, rest = n - full;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, d + i);
    memcpy(last, d + full, rest);
    last[rest] = 0x80;
    size_t tail = rest < 56 ? 64 : 128;
    unsigned long long bits = (unsigned long long)n * 8;
    for (int i = 0; i < 8; i++) last[tail - 1 - i] = (unsigned char)(bits >> (i * 8));
    sha256_block(h, last);
    if (tail == 128) sha256_block(h, last + 64);
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08X", h[i]);
}

/* v1r: the launcher's own file (for updating itself) */
bool self_exe(char *out)
{
#ifdef _WIN32
    wchar_t w[LPATH];
    DWORD n = GetModuleFileNameW(NULL, w, LPATH);
    if (!n || n >= LPATH) return false;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, LPATH, NULL, NULL) > 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, LPATH - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
#endif
}

void make_executable(const char *p)
{
#ifndef _WIN32
    chmod(p, 0755);
#else
    (void)p;
#endif
}
