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
    char game[LPATH];     /* base/jsrf or as set: default.xbe, and the built game */
    char build[LPATH];    /* base/build */
} Paths;

extern Paths g_paths;
extern Ini   g_launcher_ini;   /* launcher.ini: the launcher's own settings */

void paths_init(void);
void paths_set_game(const char *dir);     /* and saved in launcher.ini */
void paths_refresh_source(void);          /* after Update */
/* jsrf_recomp.pdb next to the game while Logging is on (Windows) */
bool debug_files_wanted(void);
int  debug_files_sync(void);              /* copies or removes it: 1 there, 0 not wanted, -1 missing */
bool debug_files_missing(void);           /* wanted but not there */
bool debug_menu_pref(void);
bool close_on_play_pref(void);
void launcher_pref_set(const char *key, const char *val);               /* DEBUG_MENU=1: the Debug tab */

/* what is there */
bool have_xbe(void);
const char *game_xbe_name(void);   /* v2c: "SegaJSRF.xbe" on the combo disc, else "default.xbe" */
void game_xbe(char *out);
const char *xbe_release(void);   /* v1x: e.g. "PAL (Europe)", NULL: no default.xbe */
bool have_game_exe(void);
bool have_tools(void);
bool have_gen(void);                      /* generated code is there (maybe not current) */
/* The Play tab's state of the generated code and of the game: 0 none,
 * 1 there but out of date, 2 up to date. Checked by req_refresh (it reads
 * the files: at start, after a job, when the game folder changes). */
void req_refresh(void);
int  req_gen(void);
int  req_build(void);
void game_exe(char *out);
void game_ini(char *out);
const char *source_id(void);
const char *update_branch(void);          /* BRANCH= in launcher.ini, else main */              /* the commit the source is at, or "" */

/* jobs */
/* JOB_GENERATE and JOB_BUILD first check whether anything changed (force:
 * do it anyway); JOB_ALL is both; JOB_UPDATE only downloads. */
enum { JOB_NONE, JOB_TOOLS, JOB_GENERATE, JOB_BUILD, JOB_ALL, JOB_UPDATE, JOB_CHECK, JOB_PYTOOL,
       JOB_SELF };                          /* v1r: the launcher updates itself */
/* the debug menu's checks: Python scripts in the project's tools folder */
enum { TOOL_CHECK_GEN, TOOL_CHECK_XBE, TOOL_COMPARE_CACHE };
bool job_start_tool(int tool);
bool job_start(int job, bool force);
bool job_busy(void);
int  job_current(void);
void job_cancel(void);
/* the last job's result: 1 ok, 0 failed, -1 none yet */
int  job_result(void);
const char *job_status(void);             /* one line: what is happening */
const char *job_phase(void);              /* the step, for the spinner: "Building game"... */
const char *update_remote(void);          /* the newest commit found by JOB_CHECK, or "" */
const char *launcher_newer(void);         /* v1r: a newer launcher's release tag found by JOB_CHECK, or "" */
bool launcher_restart_wanted(void);       /* v1r: JOB_SELF put a new launcher in place */
void launcher_cleanup_old(void);          /* v1r: at start, the file a self-update left */

bool play(void);
bool play_watchdog(int secs);             /* the game stops after secs, with a dump */
