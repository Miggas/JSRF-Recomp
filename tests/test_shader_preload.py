"""Exercise ownership of preloaded OpenGL programs without a GPU or game assets."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


KERNEL = Path(__file__).resolve().parents[1] / "xboxrecomp/src/kernel"
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned int GLuint;
static unsigned loads;
static GLuint link_with2(const char *vs, const char *fs)
{
    ++loads;
    return strcmp(fs, "bad") == 0 ? 0 : loads;
}
#include "nv2a_gl_preload.h"

int main(void)
{
    char path[64];
    assert(preloaded_take("a") == 0);
    assert(preload_program("a", "vs", "fs"));
    assert(preload_program("a", "vs", "fs"));
    assert(preload_program("b", "vs", "fs"));
    assert(loads == 2);
    assert(preloaded_take("a") == 1);
    assert(preloaded_take("a") == 0);
    assert(preloaded_take("b") == 2);
    assert(!preload_program("c", "vs", "bad"));
    assert(preloaded_take("c") == 0);
    assert(preload_program("c", "vs", "fixed"));
    assert(preloaded_take("c") == 4);
    for (unsigned i = 0; i < PRELOAD_LIMIT; ++i) {
        snprintf(path, sizeof path, "shader_cache/%u.bin", i);
        assert(preload_program(path, "vs", "fs"));
    }
    assert(!preload_program("over-limit", "vs", "fs"));
    assert(loads == 4 + PRELOAD_LIMIT);
    assert(preloaded_take("shader_cache/0.bin") == 5);
    assert(preload_program("over-limit", "vs", "fs"));
    return 0;
}
'''


class ShaderPreloadTest(unittest.TestCase):
    def test_preload_finishes_before_guest_entry(self):
        # A 23-second first-flip hold previously expired the guest's boot load.
        # Verify the barrier on both sides and guard against moving work back
        # to that first flip. The full-screen A/B run exercises boot itself.
        layout = (KERNEL / "xbox_memory_layout.c").read_text(encoding="utf-8")
        worker = layout.split("static DWORD WINAPI nv2a_ack_thread(LPVOID param)\n{", 1)[1]
        worker = worker.split("\n}", 1)[0]
        self.assertLess(worker.index("nv2a_gl_preload_startup();"), worker.index("SetEvent("))
        self.assertLess(worker.index("SetEvent("), worker.index("while (!Interlocked"))
        init = layout.split("BOOL xbox_MemoryLayoutInit(", 1)[1].split("\n}", 1)[0]
        barrier = init[init.index("xbox_WatchInit();"):]
        self.assertLess(barrier.index("xbox_Nv2aAckStart();"), barrier.index("WaitForMultipleObjects("))
        self.assertLess(barrier.index("WaitForMultipleObjects("), barrier.index("return TRUE;"))
        main = (KERNEL.parents[2] / "jsrf_recomp/src/main.c").read_text(encoding="utf-8")
        self.assertLess(main.index("if (!xbox_MemoryLayoutInit("), main.index("    xbe_entry_point();"))
        renderer = (KERNEL / "nv2a_gl.c").read_text(encoding="utf-8")
        hold = renderer.split("void nv2a_gl_prebuild_hold(void)\n{", 1)[1].split("\n}", 1)[0]
        self.assertNotIn("RCP_PRELOAD", hold)
        self.assertNotIn("nv2a_gl_preload_startup", hold)

    def test_programs_transfer_once(self):
        compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("zig")
        self.assertTrue(compiler, "Set CC to a C compiler or Zig executable.")
        command = [compiler]
        if Path(compiler).stem == "zig":
            command.append("cc")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "check.c"
            binary = source.with_suffix(".exe" if os.name == "nt" else ".out")
            source.write_text(HARNESS, encoding="utf-8")
            env = os.environ.copy()
            if Path(compiler).stem == "zig":
                env["ZIG_GLOBAL_CACHE_DIR"] = str(source.parent / "zig-global")
                env["ZIG_LOCAL_CACHE_DIR"] = str(source.parent / "zig-local")
            subprocess.run([*command, "-I", KERNEL, source, "-o", binary],
                           cwd=directory, env=env, check=True)
            subprocess.run([binary], cwd=directory, check=True)


if __name__ == "__main__":
    unittest.main()
