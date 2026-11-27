/* vk12e: the log's file name says which renderer drew and when the game
 * started (Idea by Miggas): RECOMP_LOG=stderr.txt writes
 * stderr_vk_2026-11-18_21-45-00.txt (or _gl_, _glvk_, _sw_), so runs on
 * OpenGL and Vulkan no longer overwrite each other. RECOMP_LOG_PLAIN=1 keeps
 * the name as given. The renderer comes from RECOMP_RENDERER, read the way
 * the renderer reads it (nv2a_vk_native / nv2a_vk_renderer / init_gl). */
#ifndef JSRF_LOGNAME_H
#define JSRF_LOGNAME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *jsrf_log_renderer(void)
{
    const char *r = getenv("RECOMP_RENDERER");
    if (!r || !*r) return "gl";
    if (!strcmp(r, "vulkan") || !strcmp(r, "vk")) return "vk";
    if (!strcmp(r, "glvk")) return "glvk";
    if (!strcmp(r, "sw") || !strcmp(r, "software")) return "sw";
    return "gl";
}

/* out = log with _<renderer>_<date>_<time> before its extension */
static const char *jsrf_log_name(const char *log, char *out, size_t n)
{
    const char *plain = getenv("RECOMP_LOG_PLAIN");
    const char *slash, *dot;
    char stamp[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (plain && plain[0] == '1') return log;
    if (!tm || !strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", tm))
        snprintf(stamp, sizeof stamp, "%lld", (long long)t);
    slash = strrchr(log, '/');
    {
        const char *b = strrchr(log, '\\');
        if (b && (!slash || b > slash)) slash = b;
    }
    dot = strrchr(slash ? slash + 1 : log, '.');
    if (!dot || dot == (slash ? slash + 1 : log)) dot = log + strlen(log);
    snprintf(out, n, "%.*s_%s_%s%s", (int)(dot - log), log,
             jsrf_log_renderer(), stamp, dot);
    return out;
}

#endif
