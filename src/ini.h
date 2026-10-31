/* JSRF launcher: jsrf.ini, edited in place. Every line is kept as it is
 * (comments, order, blank lines); only the settings that were changed are
 * rewritten, as KEY=VALUE (on) or ;KEY=VALUE (off). */
#pragma once
#include <stdbool.h>

enum { L_TEXT, L_SECTION, L_DESC, L_SETTING };

typedef struct {
    int  kind;
    char *text;        /* the line as it was read (no line ending) */
    /* L_SECTION / L_DESC: what to show */
    char show[512];
    /* L_SETTING */
    char key[64];
    char val[256];
    bool on;
    char val0[256];    /* as read, to see what changed */
    bool on0;
} IniLine;

typedef struct {
    IniLine *l;
    int n;
    bool crlf;
    char path[4096];
    bool loaded;
} Ini;

bool ini_load(Ini *ini, const char *path);
bool ini_save(Ini *ini);
void ini_free(Ini *ini);
bool ini_dirty(const Ini *ini);
void ini_revert(Ini *ini);
/* turns line i on and the other lines of the same key off */
void ini_set_on(Ini *ini, int i, bool on);
/* turns every line of a key off */
void ini_off(Ini *ini, const char *key);
/* the value a setting has now (the line that is on), or NULL */
const char *ini_get(const Ini *ini, const char *key);
/* sets a key: the line that is on, else the first line of that key, else a
 * new line at the end of section (or the file) */
void ini_put(Ini *ini, const char *key, const char *val, const char *section);
