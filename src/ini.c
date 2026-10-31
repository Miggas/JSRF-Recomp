/* JSRF launcher: jsrf.ini, edited in place (see ini.h). */
#include "ini.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_line(Ini *ini, const char *s, size_t n)
{
    ini->l = realloc(ini->l, sizeof(IniLine) * (ini->n + 1));
    IniLine *L = &ini->l[ini->n++];
    memset(L, 0, sizeof *L);
    L->text = malloc(n + 1);
    memcpy(L->text, s, n);
    L->text[n] = 0;
}

/* KEY=VALUE or ;KEY=VALUE, KEY in capitals */
static bool parse_setting(IniLine *L)
{
    const char *s = L->text;
    while (*s == ' ' || *s == '\t') s++;
    bool on = true;
    if (*s == ';') { on = false; s++; }
    if (!(*s >= 'A' && *s <= 'Z')) return false;
    const char *k = s;
    while ((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_') s++;
    if (*s != '=' || s - k < 3 || s - k >= (int)sizeof L->key) return false;
    memcpy(L->key, k, (size_t)(s - k));
    L->key[s - k] = 0;
    s++;
    snprintf(L->val, sizeof L->val, "%s", s);
    /* trailing spaces are not part of the value */
    size_t n = strlen(L->val);
    while (n && (L->val[n - 1] == ' ' || L->val[n - 1] == '\t')) L->val[--n] = 0;
    L->on = on;
    L->kind = L_SETTING;
    memcpy(L->val0, L->val, sizeof L->val);
    L->on0 = on;
    return true;
}

static void classify(Ini *ini)
{
    bool in_section = false;
    for (int i = 0; i < ini->n; i++) {
        IniLine *L = &ini->l[i];
        const char *s = L->text;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            const char *e = strchr(s, ']');
            if (e) {
                L->kind = L_SECTION;
                snprintf(L->show, sizeof L->show, "%.*s", (int)(e - s - 1), s + 1);
                in_section = true;
                continue;
            }
        }
        if (parse_setting(L)) continue;
        /* the banner above the first section is not shown */
        if (*s == ';' && in_section) {
            s++;
            int sp = 0;
            while (*s == ' ' && sp < 5) { s++; sp++; }
            if (!strncmp(s, "---", 3)) {
                s += 3;
                while (*s == ' ') s++;
            }
            if (*s) {
                L->kind = L_DESC;
                snprintf(L->show, sizeof L->show, "%s", s);
            }
        }
    }
}

bool ini_load(Ini *ini, const char *path)
{
    ini_free(ini);
    snprintf(ini->path, sizeof ini->path, "%s", path);
    size_t n;
    char *d = read_file(path, &n);
    if (!d) return false;
    ini->crlf = strstr(d, "\r\n") != NULL;
    size_t a = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || d[i] == '\n') {
            size_t e = i;
            if (e > a && d[e - 1] == '\r') e--;
            if (i < n || e > a) add_line(ini, d + a, e - a);
            a = i + 1;
        }
    }
    free(d);
    classify(ini);
    ini->loaded = true;
    return true;
}

static void line_text(const IniLine *L, char *out, size_t cap)
{
    if (L->kind == L_SETTING && (L->on != L->on0 || strcmp(L->val, L->val0)))
        snprintf(out, cap, "%s%s=%s", L->on ? "" : ";", L->key, L->val);
    else
        snprintf(out, cap, "%s", L->text);
}

bool ini_save(Ini *ini)
{
    if (!ini->loaded) return false;
    size_t cap = 1024, len = 0;
    char *b = malloc(cap);
    const char *nl = ini->crlf ? "\r\n" : "\n";
    for (int i = 0; i < ini->n; i++) {
        char t[1024];
        line_text(&ini->l[i], t, sizeof t);
        size_t m = strlen(t) + 2;
        if (len + m + 1 > cap) { cap = (len + m) * 2; b = realloc(b, cap); }
        memcpy(b + len, t, m - 2);
        len += m - 2;
        memcpy(b + len, nl, strlen(nl));
        len += strlen(nl);
    }
    /* written next to it first, so a failed write cannot lose the file */
    char tmp[LPATH];
    snprintf(tmp, sizeof tmp, "%s.new", ini->path);
    bool ok = write_file(tmp, b, len) && move_path(tmp, ini->path);
    free(b);
    if (ok) {
        char path[LPATH];
        snprintf(path, sizeof path, "%s", ini->path);
        ini_load(ini, path);
    }
    return ok;
}

void ini_free(Ini *ini)
{
    for (int i = 0; i < ini->n; i++) free(ini->l[i].text);
    free(ini->l);
    ini->l = NULL;
    ini->n = 0;
    ini->loaded = false;
}

bool ini_dirty(const Ini *ini)
{
    for (int i = 0; i < ini->n; i++) {
        const IniLine *L = &ini->l[i];
        if (L->kind == L_SETTING && (L->on != L->on0 || strcmp(L->val, L->val0))) return true;
    }
    return false;
}

void ini_revert(Ini *ini)
{
    for (int i = 0; i < ini->n; i++) {
        IniLine *L = &ini->l[i];
        if (L->kind == L_SETTING) {
            L->on = L->on0;
            memcpy(L->val, L->val0, sizeof L->val);
        }
    }
}

void ini_set_on(Ini *ini, int i, bool on)
{
    IniLine *L = &ini->l[i];
    if (on)
        for (int j = 0; j < ini->n; j++)
            if (j != i && ini->l[j].kind == L_SETTING && !strcmp(ini->l[j].key, L->key))
                ini->l[j].on = false;
    L->on = on;
}

void ini_off(Ini *ini, const char *key)
{
    for (int i = 0; i < ini->n; i++)
        if (ini->l[i].kind == L_SETTING && !strcmp(ini->l[i].key, key))
            ini->l[i].on = false;
}

const char *ini_get(const Ini *ini, const char *key)
{
    for (int i = 0; i < ini->n; i++)
        if (ini->l[i].kind == L_SETTING && ini->l[i].on && !strcmp(ini->l[i].key, key))
            return ini->l[i].val;
    return NULL;
}

void ini_put(Ini *ini, const char *key, const char *val, const char *section)
{
    int first = -1;
    for (int i = 0; i < ini->n; i++) {
        IniLine *L = &ini->l[i];
        if (L->kind != L_SETTING || strcmp(L->key, key)) continue;
        if (L->on) {
            snprintf(L->val, sizeof L->val, "%s", val);
            return;
        }
        if (first < 0) first = i;
    }
    if (first >= 0) {
        snprintf(ini->l[first].val, sizeof ini->l[first].val, "%s", val);
        ini_set_on(ini, first, true);
        return;
    }
    /* a new line: at the end of the section, or of the file */
    int at = ini->n;
    if (section) {
        int s = -1;
        for (int i = 0; i < ini->n; i++)
            if (ini->l[i].kind == L_SECTION && !strcmp(ini->l[i].show, section)) s = i;
        if (s >= 0) {
            at = s + 1;
            while (at < ini->n && ini->l[at].kind != L_SECTION) at++;
            while (at > s + 1 && ini->l[at - 1].text[0] == 0) at--;   /* before the blank lines */
        } else {
            char t[256];
            snprintf(t, sizeof t, "[%s]", section);
            add_line(ini, "", 0);
            add_line(ini, t, strlen(t));
            ini->l[ini->n - 1].kind = L_SECTION;
            snprintf(ini->l[ini->n - 1].show, sizeof ini->l[0].show, "%s", section);
            at = ini->n;
        }
    }
    char t[512];
    snprintf(t, sizeof t, ";%s=", key);   /* "off" as read: saving writes it on */
    add_line(ini, t, strlen(t));
    IniLine nl = ini->l[ini->n - 1];
    memmove(&ini->l[at + 1], &ini->l[at], sizeof(IniLine) * (size_t)(ini->n - 1 - at));
    ini->l[at] = nl;
    parse_setting(&ini->l[at]);
    snprintf(ini->l[at].val, sizeof ini->l[at].val, "%s", val);
    ini->l[at].on = true;
}
