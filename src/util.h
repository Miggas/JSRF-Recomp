/* JSRF launcher: files, the log, running programs. Paths are UTF-8. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define LPATH 4096

#ifdef _WIN32
#define EXE ".exe"
#define SEP "\\"
#else
#define EXE ""
#define SEP "/"
#endif

/* ---- paths and files ---- */
void  pjoin(char *out, const char *a, const char *b);          /* out = a/b (LPATH) */
void  pjoin3(char *out, const char *a, const char *b, const char *c);
void  exe_dir(char *out);                                       /* folder of this program */
bool  is_file(const char *p);
bool  is_dir(const char *p);
bool  make_dirs(const char *p);
bool  copy_file(const char *src, const char *dst);
bool  remove_file(const char *p);
bool  remove_tree(const char *p);
bool  move_path(const char *src, const char *dst);
char *read_file(const char *p, size_t *len);                    /* malloc'd, 0-terminated */
bool  write_file(const char *p, const void *data, size_t len);
/* calls cb for each entry of dir (not . or ..) */
void  list_dir(const char *dir, void (*cb)(const char *name, bool isdir, void *u), void *u);
void  set_env(const char *k, const char *v);

/* ---- fingerprints ---- */
/* A 64-bit hash of the files at root/items[i] (files or whole folders,
 * NULL-terminated list): their paths and contents, in name order, so the
 * same files give the same value on any system. Python caches, .git and the
 * launcher's own launcher_*.txt files are left out. Missing items count as
 * missing (they change the value). */
unsigned long long fingerprint(const char *root, const char *const *items,
                               const char *const *skip, unsigned long long seed);
/* skip: paths (relative to root, as in items, "/" separated) left out, or NULL */
unsigned long long hash_text(const char *s, unsigned long long seed);
#define HASH_SEED 0xcbf29ce484222325ULL

/* ---- the log (thread safe) ---- */
void  log_init(void);
void  log_text(const char *s, size_t n);     /* raw program output; \r rewrites the line */
void  logf_(const char *fmt, ...);           /* one line */
void  log_clear(void);
/* the UI copies the lines out under the lock */
int   log_lock(const char ***lines);         /* returns the line count */
void  log_unlock(void);
unsigned log_serial(void);                   /* changes whenever the log does */

/* ---- programs ---- */
/* Runs argv (argv[0] a path or a name on PATH) in cwd, its output going to
 * the log. Returns the exit code, -1 if it could not start, -2 if cancelled
 * (*cancel set). */
int   run(const char *const *argv, const char *cwd, volatile int *cancel);
/* run, with the output in out instead of the log (trailing newline cut) */
int   run_capture(const char *const *argv, const char *cwd, char *out, size_t cap);
/* Starts a program on its own (the game), not waiting for it. */
bool  launch(const char *exe, const char *cwd);
/* Opens a folder in the file manager. */
void  open_folder(const char *p);
int   cpu_count(void);
