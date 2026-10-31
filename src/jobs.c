/* JSRF launcher: where things are, and the jobs (see jobs.h).
 *
 * The tools are portable copies in tools/, downloaded once (about 400 MB
 * unpacked), so nothing has to be installed:
 *   Python   python-build-standalone (a full Python in a folder)
 *   capstone the translator's disassembler   } from PyPI, with that
 *   Zig      the C compiler (clang inside)   } Python's pip
 *   CMake, Ninja  the build system          }
 * On Windows, Visual Studio is used instead of Zig when it is installed and
 * chosen (the compiler the project has always been built with). */
#include "jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include <SDL.h>


Paths g_paths;
Ini   g_launcher_ini;

#define REPO        "Miggas/JSRF-Recomp"
#define PY_RELEASE  "20241016"
#define PY_VERSION  "3.12.7"
#ifdef _WIN32
#define PY_PLATFORM "x86_64-pc-windows-msvc"
#else
#define PY_PLATFORM "x86_64-unknown-linux-gnu"
#endif
/* capstone: the version the generated code is made and tested with */
static const char *const PIP_PACKAGES[] = {
    "capstone==5.0.7", "ziglang==0.13.0", "cmake==3.31.6", "ninja==1.11.1.4", NULL
};
/* written to tools/toolchain.txt: a different line sets the tools up again */
#define TOOLCHAIN_ID "py=" PY_VERSION " capstone=5.0.7 zig=0.13.0 cmake=3.31.6 ninja=1.11.1.4 wrap=1"
/* the compiler wrappers' own version: a different one makes them again
 * (only them: Python and the packages stay) */
#define WRAP_ID "wrap=2"

/* ======================================================================
 * paths
 * ==================================================================== */
static bool is_source(const char *d)
{
    char a[LPATH], b[LPATH];
    pjoin3(a, d, "jsrf_recomp", "CMakeLists.txt");
    pjoin3(b, d, "xboxrecomp", "CMakeLists.txt");
    return is_file(a) && is_file(b);
}

static void launcher_ini_save(void)
{
    if (ini_dirty(&g_launcher_ini)) ini_save(&g_launcher_ini);
}

void paths_init(void)
{
    Paths *p = &g_paths;
    memset(p, 0, sizeof *p);
    exe_dir(p->base);
    pjoin(p->tools, p->base, "tools");
    pjoin(p->build, p->base, "build");

    char li[LPATH];
    pjoin(li, p->base, "launcher.ini");
    if (!is_file(li)) {
        const char *t =
            "; JSRF launcher settings (the game's own are in jsrf.ini, in the game folder)\n"
            "[launcher]\n"
            "; --- The project's branch Update downloads: main (default) or test (newest, less tested).\n"
            ";BRANCH=test\n";
        write_file(li, t, strlen(t));
    }
    ini_load(&g_launcher_ini, li);

    /* one layout: the project in source/, the game files in game/ (or
     * the folder set in Play) */
    pjoin(p->source, p->base, "source");
    p->have_source = is_source(p->source);

    /* the game folder: as set in Play, else game/ */
    const char *gd = ini_get(&g_launcher_ini, "GAME_DIR");
    if (gd && *gd) snprintf(p->game, sizeof p->game, "%s", gd);
    else pjoin(p->game, p->base, "game");
}

void paths_set_game(const char *dir)
{
    snprintf(g_paths.game, sizeof g_paths.game, "%s", dir);
    size_t n = strlen(g_paths.game);
    while (n > 1 && (g_paths.game[n - 1] == '/' || g_paths.game[n - 1] == '\\')) g_paths.game[--n] = 0;
    ini_put(&g_launcher_ini, "GAME_DIR", g_paths.game, "launcher");
    launcher_ini_save();
}

int compiler_pref(void)
{
    const char *c = ini_get(&g_launcher_ini, "COMPILER");
    if (c && !strcmp(c, "zig")) return COMP_ZIG;
    if (c && !strcmp(c, "msvc")) return COMP_MSVC;
    return COMP_AUTO;
}

void compiler_set(int c)
{
    ini_put(&g_launcher_ini, "COMPILER", c == COMP_ZIG ? "zig" : c == COMP_MSVC ? "msvc" : "auto",
            "launcher");
    launcher_ini_save();
}

/* ======================================================================
 * what is there
 * ==================================================================== */
static void py_exe(char *out)
{
#ifdef _WIN32
    pjoin3(out, g_paths.tools, "python", "python.exe");
#else
    char t[LPATH];
    pjoin3(t, g_paths.tools, "python", "bin");
    pjoin(out, t, "python3");
#endif
}

static void site_dir(char *out)
{
#ifdef _WIN32
    char t[LPATH];
    pjoin3(t, g_paths.tools, "python", "Lib");
    pjoin(out, t, "site-packages");
#else
    char t[LPATH];
    pjoin3(t, g_paths.tools, "python", "lib");
    pjoin3(out, t, "python3.12", "site-packages");
#endif
}

/* a package's own program: site-packages/<pkg>/data/bin/<name> (the
 * Scripts/ launchers pip makes hold absolute paths, which break if the
 * folder is moved) */
static void pkg_bin(char *out, const char *pkg, const char *name)
{
    char s[LPATH], t[LPATH];
    site_dir(s);
    pjoin3(t, s, pkg, "data");
    pjoin(s, t, "bin");
    snprintf(t, sizeof t, "%s" EXE, name);
    pjoin(out, s, t);
}

/* Ninja's package puts the program itself in Python's scripts folder */
static void ninja_exe(char *out)
{
#ifdef _WIN32
    pjoin3(out, g_paths.tools, "python", "Scripts");
    pjoin(out, out, "ninja.exe");
#else
    pjoin3(out, g_paths.tools, "python", "bin");
    pjoin(out, out, "ninja");
#endif
}

static void zig_exe(char *out)
{
    char s[LPATH];
    site_dir(s);
    pjoin3(out, s, "ziglang", "zig" EXE);
}

static void wrap_dir(char *out) { pjoin(out, g_paths.tools, "zigwrap"); }

bool have_tools(void)
{
    char t[LPATH];
    pjoin(t, g_paths.tools, "toolchain.txt");
    char *d = read_file(t, NULL);
    bool ok = d && !strncmp(d, TOOLCHAIN_ID, strlen(TOOLCHAIN_ID));
    free(d);
    return ok;
}

void game_exe(char *out) { pjoin(out, g_paths.game, "jsrf_recomp" EXE); }
void game_ini(char *out) { pjoin(out, g_paths.game, "jsrf.ini"); }

bool have_xbe(void)
{
    char x[LPATH];
    pjoin(x, g_paths.game, "default.xbe");
    return is_file(x);
}

bool have_game_exe(void)
{
    char x[LPATH];
    game_exe(x);
    return is_file(x);
}

#ifdef _WIN32
static bool vswhere(char *out, size_t cap)
{
    const char *pf = getenv("ProgramFiles(x86)");
    if (!pf) return false;
    char vw[LPATH];
    snprintf(vw, sizeof vw, "%s\\Microsoft Visual Studio\\Installer\\vswhere.exe", pf);
    if (!is_file(vw)) return false;
    const char *argv[] = { vw, "-latest", "-products", "*", "-requires",
                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                           "-property", "installationPath", NULL };
    return run_capture(argv, NULL, out, cap) == 0 && out[0];
}
#endif

bool have_msvc(void)
{
#ifdef _WIN32
    static int v = -1;
    if (v < 0) {
        char o[LPATH];
        v = vswhere(o, sizeof o);
    }
    return v == 1;
#else
    return false;
#endif
}

static bool use_msvc(void)
{
    int c = compiler_pref();
    if (c == COMP_ZIG) return false;
    if (c == COMP_MSVC) return true;
    return have_msvc();
}

/* BRANCH= in launcher.ini: the project's branch Update downloads.
 * Missing, empty or not a branch name: main. */
const char *update_branch(void)
{
    static char b[100];
    const char *v = ini_get(&g_launcher_ini, "BRANCH");
    bool ok = v && *v && strlen(v) < sizeof b;
    for (const char *c = v; ok && *c; c++)
        ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
             *c == '-' || *c == '_' || *c == '.' || *c == '/';
    snprintf(b, sizeof b, "%s", ok ? v : "main");
    return b;
}

static char s_source_id[64];

const char *source_id(void)
{
    char f[LPATH];
    pjoin(f, g_paths.source, "launcher_commit.txt");
    char *d = read_file(f, NULL);
    snprintf(s_source_id, sizeof s_source_id, "%s", d ? d : "");
    free(d);
    char *e = strpbrk(s_source_id, "\r\n ");
    if (e) *e = 0;
    return s_source_id;
}

/* ======================================================================
 * job thread
 * ==================================================================== */
static SDL_Thread *s_thread;
static SDL_atomic_t s_busy;
static int s_job, s_result = -1;
static volatile int s_cancel;
static bool s_regen;
static char s_status[256];
static char s_remote[64];

const char *job_status(void) { return s_status; }
const char *update_remote(void) { return s_remote; }
bool job_busy(void) { return SDL_AtomicGet(&s_busy) != 0; }
int  job_current(void) { return job_busy() ? s_job : JOB_NONE; }
int  job_result(void) { return s_result; }
void job_cancel(void) { s_cancel = 1; }

static void status(const char *fmt, const char *a)
{
    snprintf(s_status, sizeof s_status, fmt, a ? a : "");
    logf_("== %s", s_status);
}

static bool runv(const char *cwd, const char *const *argv)
{
    char line[2048] = "";
    for (int i = 0; argv[i]; i++) {
        size_t n = strlen(line);
        snprintf(line + n, sizeof line - n, i ? " %s" : "%s", argv[i]);
    }
    logf_("> %s", line);
    int r = run(argv, cwd, &s_cancel);
    if (r == -2) { logf_("Cancelled."); return false; }
    if (r != 0) { logf_("Failed (exit code %d).", r); return false; }
    return true;
}

static bool cancelled(void) { return s_cancel != 0; }

/* ---- tools ---- */

/* The wrapper programs CMake runs as its compiler: zig-cc -> "zig cc" ...
 * They find Zig relative to themselves, so the folder can be moved. */
static const char ZIGWRAP_C[] =
"/* zig-cc, zig-c++, zig-ar, zig-ranlib: run \"zig <ZIG_MODE>\" with the\n"
" * same arguments. Made by the JSRF launcher. The mode is built in, not\n"
" * read from the file name: Windows may start it by its short name\n"
" * (ZIG-C_~1.EXE) when the path has spaces. */\n"
"#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
"#ifdef _WIN32\n#include <windows.h>\n"
"int main(void) {\n"
"    wchar_t self[4096], zig[4096];\n"
"    const wchar_t *mode = L\"\" ZIG_MODE;\n"
"    DWORD n = GetModuleFileNameW(NULL, self, 4096);\n"
"    if (!n || n >= 4096) return 1;\n"
"    wchar_t *base = wcsrchr(self, L'\\\\'); base = base ? base + 1 : self;\n"
"    *base = 0;\n"
"    _snwprintf(zig, 4096, L\"%ls%ls\", self, L\"\" ZIG_REL); zig[4095] = 0;\n"
"    /* the arguments as given, after our own name */\n"
"    wchar_t *cl = GetCommandLineW();\n"
"    if (*cl == L'\"') { cl++; while (*cl && *cl != L'\"') cl++; if (*cl) cl++; }\n"
"    else while (*cl && *cl != L' ' && *cl != L'\\t') cl++;\n"
"    size_t len = wcslen(zig) + wcslen(mode) + wcslen(cl) + 8;\n"
"    wchar_t *cmd = malloc(len * sizeof(wchar_t));\n"
"    _snwprintf(cmd, len, L\"\\\"%ls\\\" %ls%ls\", zig, mode, cl); cmd[len - 1] = 0;\n"
"    STARTUPINFOW si; PROCESS_INFORMATION pi;\n"
"    memset(&si, 0, sizeof si); si.cb = sizeof si;\n"
"    if (!CreateProcessW(zig, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {\n"
"        fwprintf(stderr, L\"zigwrap: cannot run %ls\\n\", zig); return 127; }\n"
"    WaitForSingleObject(pi.hProcess, INFINITE);\n"
"    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);\n"
"    return (int)code;\n"
"}\n"
"#else\n#include <unistd.h>\n"
"int main(int argc, char **argv) {\n"
"    char self[4096], zig[4200];\n"
"    ssize_t n = readlink(\"/proc/self/exe\", self, sizeof self - 1);\n"
"    if (n <= 0) return 1; self[n] = 0;\n"
"    char *slash = strrchr(self, '/'); if (slash) slash[1] = 0;\n"
"    snprintf(zig, sizeof zig, \"%s%s\", self, ZIG_REL);\n"
"    char **a = malloc(sizeof(char *) * (argc + 2));\n"
"    a[0] = zig; a[1] = (char *)ZIG_MODE;\n"
"    for (int i = 1; i < argc; i++) a[i + 1] = argv[i];\n"
"    a[argc + 1] = NULL;\n"
"    execv(zig, a);\n"
"    fprintf(stderr, \"zigwrap: cannot run %s\\n\", zig); return 127;\n"
"}\n"
"#endif\n";

static bool make_wrappers(void)
{
    char wd[LPATH], src[LPATH], zig[LPATH], out[LPATH];
    wrap_dir(wd);
    remove_tree(wd);
    make_dirs(wd);
    pjoin(src, wd, "zigwrap.c");
    if (!write_file(src, ZIGWRAP_C, strlen(ZIGWRAP_C))) return false;
    zig_exe(zig);
#ifdef _WIN32
    const char *rel = "-DZIG_REL=\"..\\\\python\\\\Lib\\\\site-packages\\\\ziglang\\\\zig.exe\"";
#else
    const char *rel = "-DZIG_REL=\"../python/lib/python3.12/site-packages/ziglang/zig\"";
#endif
    static const char *const modes[] = { "cc", "c++", "ar", "ranlib", NULL };
    for (int i = 0; modes[i]; i++) {
        char name[32], def[48];
        snprintf(name, sizeof name, "zig-%s" EXE, modes[i]);
        snprintf(def, sizeof def, "-DZIG_MODE=\"%s\"", modes[i]);
        pjoin(out, wd, name);
        const char *argv[] = { zig, "cc", "-O2", rel, def, "-o", out, src, NULL };
        if (!runv(wd, argv)) return false;
    }
    char v[LPATH];
    pjoin(v, wd, "wrap_version.txt");
    write_file(v, WRAP_ID "\n", strlen(WRAP_ID) + 1);
    return true;
}

static bool download(const char *url, const char *dest)
{
    const char *argv[] = { "curl", "-fL", "--retry", "3", "-#", "-o", dest, url, NULL };
    return runv(NULL, argv);
}

static bool wrappers_ok(void)
{
    char v[LPATH];
    wrap_dir(v);
    pjoin(v, v, "wrap_version.txt");
    char *d = read_file(v, NULL);
    bool ok = d && !strncmp(d, WRAP_ID, strlen(WRAP_ID));
    free(d);
    return ok;
}

static bool setup_tools(void)
{
    if (have_tools()) {
        if (wrappers_ok()) return true;
        status("Making the compiler wrappers again...", NULL);
        return make_wrappers();
    }
    make_dirs(g_paths.tools);
    char py[LPATH];
    py_exe(py);
    if (!is_file(py)) {
        status("Downloading Python (about 45 MB)...", NULL);
        char url[512], tgz[LPATH];
        snprintf(url, sizeof url,
                 "https://github.com/astral-sh/python-build-standalone/releases/download/"
                 PY_RELEASE "/cpython-" PY_VERSION "+" PY_RELEASE "-" PY_PLATFORM
                 "-install_only.tar.gz");
        pjoin(tgz, g_paths.tools, "python.tar.gz");
        if (!download(url, tgz)) return false;
        status("Unpacking Python...", NULL);
        const char *argv[] = { "tar", "-xzf", tgz, "-C", g_paths.tools, NULL };
        if (!runv(NULL, argv)) return false;
        remove_file(tgz);
        if (!is_file(py)) { logf_("Python was not unpacked to %s", py); return false; }
    }
    status("Downloading the translator and compiler (capstone, Zig, CMake, Ninja)...", NULL);
    const char *argv[16];
    int n = 0;
    argv[n++] = py;
    argv[n++] = "-m";
    argv[n++] = "pip";
    argv[n++] = "install";
    argv[n++] = "--disable-pip-version-check";
    argv[n++] = "--no-warn-script-location";
    for (int i = 0; PIP_PACKAGES[i]; i++) argv[n++] = PIP_PACKAGES[i];
    argv[n] = NULL;
    if (!runv(NULL, argv)) return false;
    char z[LPATH], c[LPATH], k[LPATH];
    zig_exe(z);
    pkg_bin(c, "cmake", "cmake");
    ninja_exe(k);
    if (!is_file(z) || !is_file(c) || !is_file(k)) {
        logf_("Missing after install: %s%s%s", is_file(z) ? "" : "zig ",
              is_file(c) ? "" : "cmake ", is_file(k) ? "" : "ninja");
        return false;
    }
    status("Making the compiler wrappers...", NULL);
    if (!make_wrappers()) return false;
    char t[LPATH];
    pjoin(t, g_paths.tools, "toolchain.txt");
    write_file(t, TOOLCHAIN_ID "\n", strlen(TOOLCHAIN_ID) + 1);
    logf_("Tools ready.");
    return true;
}

/* ---- build ---- */

static void clear_shader_cache_cb(const char *name, bool isdir, void *u)
{
    size_t n = strlen(name);
    if (isdir || n < 4 || strcmp(name + n - 4, ".bin") || !strcmp(name, "recipes.bin")) return;
    char p[LPATH];
    pjoin(p, (const char *)u, name);
    remove_file(p);
}

/* ---- what is up to date ---- */

static void gen_dir(char *out)
{
    char t[LPATH];
    pjoin3(t, g_paths.source, "jsrf_recomp", "src");
    pjoin3(out, t, "recomp", "gen");
}

/* What the translator reads: the game's default.xbe, the project's
 * analysis and tools, recomp_manual.c (the functions it leaves out), and
 * the toolkit's translator. The same value: the same code would come out. */
static unsigned long long gen_inputs(void)
{
    static const char *const src[] = {
        "jsrf_recomp/analysis", "jsrf_recomp/tools", "jsrf_recomp/src/recomp_manual.c",
        "xboxrecomp/tools", NULL };
    /* the translator's own work folder (it writes there while running) */
    static const char *const skip[] = { "xboxrecomp/tools/recomp/output", NULL };
    static const char *const xbe[] = { "default.xbe", NULL };
    unsigned long long h = fingerprint(g_paths.source, src, skip, HASH_SEED);
    return fingerprint(g_paths.game, xbe, NULL, h);
}

/* What the compiler reads (the generated code included), and which
 * compiler: the same value, the same game. */
static unsigned long long build_inputs(bool msvc)
{
    static const char *const src[] = {
        "jsrf_recomp/src", "jsrf_recomp/CMakeLists.txt", "jsrf_recomp/cmake",
        "xboxrecomp/src", "xboxrecomp/include", "xboxrecomp/cmake", "xboxrecomp/CMakeLists.txt",
        NULL };
    unsigned long long h = hash_text(msvc ? "msvc" : "zig " TOOLCHAIN_ID, HASH_SEED);
    return fingerprint(g_paths.source, src, NULL, h);
}

static bool stamp_is(const char *file, unsigned long long v)
{
    char *d = read_file(file, NULL);
    char want[32];
    snprintf(want, sizeof want, "%016llx", v);
    bool same = d && !strncmp(d, want, 16);
    free(d);
    return same;
}

static void stamp_write(const char *file, unsigned long long v)
{
    char t[32];
    snprintf(t, sizeof t, "%016llx\n", v);
    write_file(file, t, strlen(t));
}

static bool gen_current(void)
{
    char g[LPATH], f[LPATH], st[LPATH];
    gen_dir(g);
    pjoin(f, g, "recomp_dispatch.c");
    pjoin(st, g, "launcher_gen.txt");
    return is_file(f) && stamp_is(st, gen_inputs());
}

bool have_gen(void)
{
    char g[LPATH], f[LPATH];
    gen_dir(g);
    pjoin(f, g, "recomp_dispatch.c");
    return is_file(f);
}

static bool need_source_and_xbe(void)
{
    if (!g_paths.have_source) { logf_("The project is not downloaded yet: Update -> Download."); return false; }
    if (!have_xbe()) { logf_("default.xbe not found in %s", g_paths.game); return false; }
    return true;
}

/* ---- generate ---- */

static bool do_generate(void)
{
    if (!need_source_and_xbe()) return false;
    status("Checking the generated code...", NULL);
    if (!s_regen && gen_current()) {
        status("The generated code is up to date: nothing to do.", NULL);
        return true;
    }
    if (!setup_tools()) return false;
    if (cancelled()) return false;
    char jr[LPATH], py[LPATH], xbe[LPATH], gen[LPATH], tool[LPATH];
    pjoin(jr, g_paths.source, "jsrf_recomp");
    py_exe(py);
    pjoin(xbe, g_paths.game, "default.xbe");
    gen_dir(gen);
    /* taken before: a file changing while it runs makes it run again next time */
    unsigned long long h = gen_inputs();
    status("Translating the game's code from default.xbe (a few minutes)...", NULL);
    pjoin3(tool, jr, "tools", "generate_code.py");
    const char *argv[] = { py, tool, xbe, NULL };
    if (!runv(jr, argv)) return false;
    char st[LPATH];
    pjoin(st, gen, "launcher_gen.txt");
    stamp_write(st, h);
    status("Code generated.", NULL);
    return true;
}

/* ---- build ---- */

static bool do_build(void)
{
    if (!need_source_and_xbe()) return false;
    status("Checking the generated code...", NULL);
    if (!have_gen()) {
        logf_("No generated code yet: press Generate code first.");
        return false;
    }
    if (!gen_current()) {
        logf_("The generated code is older than the project or default.xbe: press Generate code first.");
        return false;
    }
    if (!setup_tools()) return false;
    if (cancelled()) return false;

    char jr[LPATH];
    pjoin(jr, g_paths.source, "jsrf_recomp");
    /* (wrap_dsound.py, which rebuild.bat runs before each build, is the
     * translator's last step: code that counts as current here has it) */

    bool msvc = use_msvc();
    if (!msvc) {
        char tc[LPATH];
        pjoin3(tc, jr, "cmake", "zig.cmake");
        if (!is_file(tc)) {
            logf_("This version of the project can't be built with Zig (it needs v246 or newer).");
            logf_("Update the project, or build with Visual Studio (Build -> Compiler).");
            return false;
        }
    }
    char bdir[LPATH], cmake[LPATH], ninja[LPATH], stamp[LPATH], want[LPATH * 2 + 32];
    pjoin(bdir, g_paths.build, msvc ? "msvc" : "zig");

    char built[LPATH], dest[LPATH], bstamp[LPATH];
    if (msvc) pjoin3(built, bdir, "Release", "jsrf_recomp" EXE);
    else pjoin(built, bdir, "jsrf_recomp" EXE);
    game_exe(dest);
    pjoin(bstamp, bdir, "launcher_built.txt");
    status("Checking what changed...", NULL);
    unsigned long long bh = build_inputs(msvc);
    bool up_to_date = !s_regen && is_file(built) && stamp_is(bstamp, bh);
    if (up_to_date && is_file(dest)) {
        status("The game is up to date: nothing to build.", NULL);
        return true;
    }
    if (!up_to_date) remove_file(bstamp);
    pkg_bin(cmake, "cmake", "cmake");
    ninja_exe(ninja);
    /* the build folder holds absolute paths: made again if the launcher,
     * the project or the tools moved */
    pjoin(stamp, bdir, "launcher_stamp.txt");
    snprintf(want, sizeof want, "%s|%s|" TOOLCHAIN_ID " " WRAP_ID, g_paths.source, g_paths.tools);
    {
        char *d = read_file(stamp, NULL);
        bool same = d && !strcmp(d, want);
        free(d);
        char marker[LPATH];
        pjoin(marker, bdir, msvc ? "ALL_BUILD.vcxproj" : "build.ninja");
        if (!up_to_date && (!same || !is_file(marker))) {
            remove_tree(bdir);
            make_dirs(bdir);
            status("Setting up the build (the first time takes a few minutes)...", NULL);
            char tc[LPATH], zb[LPATH], mk[LPATH + 32];
            const char *argv[24];
            int n = 0;
            argv[n++] = cmake;
            argv[n++] = "-S"; argv[n++] = jr;
            argv[n++] = "-B"; argv[n++] = bdir;
            if (msvc) {
                /* CMake picks the newest Visual Studio installed */
                argv[n++] = "-A"; argv[n++] = "x64";
            } else {
                static char d1[LPATH + 64], d2[LPATH + 32];
                pjoin3(tc, jr, "cmake", "zig.cmake");
                wrap_dir(zb);
                snprintf(d1, sizeof d1, "-DCMAKE_TOOLCHAIN_FILE=%s", tc);
                snprintf(d2, sizeof d2, "-DZIG_BIN=%s", zb);
                snprintf(mk, sizeof mk, "-DCMAKE_MAKE_PROGRAM=%s", ninja);
                argv[n++] = "-G"; argv[n++] = "Ninja";
                argv[n++] = mk;
                argv[n++] = "-DCMAKE_BUILD_TYPE=Release";
                argv[n++] = d1;
                argv[n++] = d2;
#ifndef _WIN32
                /* SDL2's released headers + the system's SDL2 (SteamOS has
                 * no development package; a distro's would mix its own C
                 * headers in with Zig's) */
                argv[n++] = "-DXBOXRECOMP_SDL2_RUNTIME=ON";
#endif
            }
            argv[n] = NULL;
            if (!runv(jr, argv)) return false;
            write_file(stamp, want, strlen(want));
        }
    }
    if (cancelled()) return false;

    if (!up_to_date) {
        status("Compiling (the first time takes a while)...", NULL);
        char par[16];
        snprintf(par, sizeof par, "%d", cpu_count());
        const char *argv[] = { cmake, "--build", bdir, "--config", "Release", "--parallel", par, NULL };
        if (!runv(jr, argv)) return false;
        stamp_write(bstamp, bh);
    }

    status("Copying the game to the game folder...", NULL);
    if (!copy_file(built, dest)) {
        logf_("Could not copy the game to %s (is the game still running?)", dest);
        return false;
    }
    /* the debug symbols too, so crash reports and RECOMP_PROFILE can name
     * the game's functions (Zig's exe looks for it next to itself) */
    {
        char bp[LPATH], gp[LPATH];
        if (msvc) pjoin3(bp, bdir, "Release", "jsrf_recomp.pdb");
        else pjoin(bp, bdir, "jsrf_recomp.pdb");
        pjoin(gp, g_paths.game, "jsrf_recomp.pdb");
        if (is_file(bp) && !copy_file(bp, gp)) logf_("Could not copy %s", gp);
    }
    char ini[LPATH];
    game_ini(ini);
    if (!is_file(ini)) {
        char tmpl[LPATH];
        pjoin3(tmpl, jr, "portable", "jsrf.ini");
        if (copy_file(tmpl, ini)) logf_("Created %s", ini);
    } else {
        logf_("Kept your %s", ini);
    }
    /* the new build may make its shaders differently: the compiled ones
     * go, recipes.bin stays (they are compiled again in the menus) */
    char sc[LPATH];
    pjoin(sc, g_paths.game, "shader_cache");
    if (is_dir(sc)) {
        list_dir(sc, clear_shader_cache_cb, sc);
        logf_("Cleared the shader cache (kept recipes.bin).");
    }
    status("Build ready: %s", dest);
    return true;
}

/* ---- update ---- */

static bool check_update(void)
{
    s_remote[0] = 0;
    status("Checking for updates...", NULL);
    char url[256];
    snprintf(url, sizeof url, "https://api.github.com/repos/" REPO "/commits/%s", update_branch());
    const char *argv[] = { "curl", "-fsSL", "-H", "Accept: application/vnd.github.sha", url, NULL };
    char out[256];
    if (run_capture(argv, NULL, out, sizeof out) != 0 || strlen(out) < 40) {
        logf_("Could not reach GitHub. %s", out);
        return false;
    }
    out[40] = 0;
    snprintf(s_remote, sizeof s_remote, "%s", out);
    if (!strcmp(s_remote, source_id())) status("Up to date (%s).", s_remote);
    else status("Update available: %s", s_remote);
    return true;
}

static bool do_update(void)
{
    /* the newest commit, when GitHub answers: nothing to download if it is
     * the one here (no answer: download anyway) */
    if (g_paths.have_source && *source_id() && check_update() && !strcmp(s_remote, source_id())) {
        status("Already the newest version (%.10s): nothing to download.", s_remote);
        return true;
    }
    if (!setup_tools()) return false;
    char dl[LPATH], zip[LPATH], tmp[LPATH], py[LPATH];
    pjoin(dl, g_paths.tools, "download");
    remove_tree(dl);
    make_dirs(dl);
    pjoin(zip, dl, "source.zip");
    pjoin(tmp, dl, "unpacked");
    py_exe(py);
    status("Downloading the newest version of the project (%s)...", update_branch());
    char url[256];
    snprintf(url, sizeof url, "https://github.com/" REPO "/archive/refs/heads/%s.zip", update_branch());
    if (!download(url, zip)) return false;
    status("Unpacking...", NULL);
    /* GitHub puts the commit in the zip's comment */
    const char *argv[] = { py, "-c",
        "import sys,zipfile\n"
        "z=zipfile.ZipFile(sys.argv[1])\n"
        "z.extractall(sys.argv[2])\n"
        "print(z.comment.decode('ascii','replace').strip())\n",
        zip, tmp, NULL };
    char sha[256];
    if (run_capture(argv, NULL, sha, sizeof sha) != 0) { logf_("Could not unpack: %s", sha); return false; }
    char top[LPATH];
    /* the zip's folder: repo-branch, a / in the branch name becoming - */
    char topname[160];
    snprintf(topname, sizeof topname, "JSRF-Recomp-%s", update_branch());
    for (char *c = topname; *c; c++) if (*c == '/') *c = '-';
    pjoin(top, tmp, topname);
    if (!is_source(top)) { logf_("The download does not look like the project."); return false; }

    char old[LPATH];
    snprintf(old, sizeof old, "%s.old", g_paths.source);
    remove_tree(old);
    if (is_dir(g_paths.source)) {
        /* game files kept in the project's own game folder come along */
        char og[LPATH], ng[LPATH];
        pjoin3(og, g_paths.source, "jsrf_recomp", "game");
        pjoin3(ng, top, "jsrf_recomp", "game");
        char ox[LPATH];
        pjoin(ox, og, "default.xbe");
        if (is_file(ox)) { remove_tree(ng); move_path(og, ng); }
        /* the generated code too: Generate code makes it again only if
         * what it is made from changed */
        char gold[LPATH], gnew[LPATH], t[LPATH];
        gen_dir(gold);
        pjoin3(t, top, "jsrf_recomp", "src");
        pjoin3(gnew, t, "recomp", "gen");
        if (is_dir(gold)) {
            /* src/recomp is not in the download (the generated code is
             * never in the repository): made first, or the move fails */
            char parent[LPATH];
            pjoin(parent, t, "recomp");
            make_dirs(parent);
            remove_tree(gnew);
            if (!move_path(gold, gnew)) logf_("Could not keep the generated code: Generate code makes it again.");
        }
        if (!move_path(g_paths.source, old)) { logf_("Could not move the old version away."); return false; }
    }
    if (!move_path(top, g_paths.source)) {
        logf_("Could not put the new version in place.");
        move_path(old, g_paths.source);
        return false;
    }
    char cf[LPATH];
    pjoin(cf, g_paths.source, "launcher_commit.txt");
    write_file(cf, sha, strlen(sha));
    remove_tree(old);
    remove_tree(dl);
    g_paths.have_source = true;
    char gd[LPATH];
    pjoin3(gd, g_paths.source, "jsrf_recomp", "game");
    if (strstr(g_paths.game, gd) == g_paths.game) logf_("Game files kept in %s", gd);
    snprintf(s_remote, sizeof s_remote, "%.40s", sha);
    status("Updated to %.10s. Then Build: Generate code (if needed) and Build.", sha);
    return true;
}

static int job_thread(void *u)
{
    (void)u;
    bool ok = false;
    switch (s_job) {
    case JOB_TOOLS:    ok = setup_tools(); break;
    case JOB_GENERATE: ok = do_generate(); break;
    case JOB_BUILD:    ok = do_build(); break;
    case JOB_ALL:      ok = do_generate() && !cancelled() && do_build(); break;
    case JOB_UPDATE: ok = do_update(); break;
    case JOB_CHECK:  ok = check_update(); break;
    }
    if (!ok && s_cancel) status("Cancelled.", NULL);
    else if (!ok && s_job != JOB_CHECK) status("Failed: the first error is in the log above.", NULL);
    s_result = ok;
    SDL_AtomicSet(&s_busy, 0);
    return 0;
}

bool job_start(int job, bool force)
{
    if (job_busy()) return false;
    if (s_thread) { SDL_WaitThread(s_thread, NULL); s_thread = NULL; }
    s_job = job;
    s_regen = force;
    s_cancel = 0;
    s_result = -1;
    SDL_AtomicSet(&s_busy, 1);
    /* the tools' own caches stay in tools/ too */
    char t[LPATH];
    pjoin(t, g_paths.tools, "zig-cache");
    set_env("ZIG_GLOBAL_CACHE_DIR", t);
    set_env("PIP_DISABLE_PIP_VERSION_CHECK", "1");
    set_env("PYTHONUTF8", "1");
    {
        static const char *const names[] = { "", "Download tools", "Generate code", "Build",
                                             "Generate code + Build", "Update", "Check for updates" };
        char tm[32];
        time_t now = time(NULL);
        strftime(tm, sizeof tm, "%H:%M:%S", localtime(&now));
        logf_("---- %s %s%s ----", tm, names[job], force ? " (even if nothing changed)" : "");
    }
    s_thread = SDL_CreateThread(job_thread, "job", NULL);
    if (!s_thread) { SDL_AtomicSet(&s_busy, 0); return false; }
    return true;
}

bool play(void)
{
    char exe[LPATH];
    game_exe(exe);
    if (!is_file(exe)) return false;
    return launch(exe, g_paths.game);
}
