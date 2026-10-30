/* JSRF launcher: where things are, and the jobs (set up the tools, build,
 * update, play). A job runs on its own thread; its output goes to the log. */
#pragma once
#include <stdbool.h>
#include "ini.h"
#include "util.h"

typedef struct {
    char base[LPATH];     /* the launcher's folder */
    char tools[LPATH];    /* base/tools: Python, Zig, CMake, Ninja */
    char source[LPATH];   /* base/source: the project, as downloaded by Update */
    bool have_source;
    char game[LPATH];     /* base/game or as set: default.xbe, and the built game */
    char build[LPATH];    /* base/build */
} Paths;

extern Paths g_paths;
extern Ini   g_launcher_ini;   /* launcher.ini: the launcher's own settings */

enum { COMP_AUTO, COMP_ZIG, COMP_MSVC };

void paths_init(void);
void paths_set_game(const char *dir);     /* and saved in launcher.ini */
int  compiler_pref(void);
void compiler_set(int c);

/* what is there */
bool have_xbe(void);
bool have_game_exe(void);
bool have_tools(void);
bool have_msvc(void);                     /* Visual Studio with the C++ tools (Windows) */
void game_exe(char *out);
void game_ini(char *out);
const char *source_id(void);              /* the commit the source is at, or "" */

/* jobs */
enum { JOB_NONE, JOB_TOOLS, JOB_BUILD, JOB_UPDATE, JOB_CHECK };
bool job_start(int job, bool regenerate);
bool job_busy(void);
int  job_current(void);
void job_cancel(void);
/* the last job's result: 1 ok, 0 failed, -1 none yet */
int  job_result(void);
const char *job_status(void);             /* one line: what is happening */
const char *update_remote(void);          /* the newest commit found by JOB_CHECK, or "" */

bool play(void);
