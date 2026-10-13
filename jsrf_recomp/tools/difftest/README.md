# difftest: original x86 vs lifted C, on Linux

Runs a guest function twice: the original bytes in Unicorn, and the lifted C
compiled with GCC into `libjsrf.so` against the stub runtime in `rt.c`. Same
memory image, same kernel model (thunk slots → 0xFE000000+4*slot). After every
call it compares the return value, esp and guest memory.

    pip install unicorn capstone
    # compile every src/recomp/gen/*.c with: gcc -std=gnu11 -O1 -w -fPIC -fno-strict-aliasing -c
    gcc -std=gnu11 -O1 -fPIC -c rt.c && gcc -shared -o libjsrf.so *.o -lm
    python3 heapdiff.py default.xbe info.json ./libjsrf.so <seed> <ops>

`heapdiff.py` creates the XAPI process heap with the game's own RtlCreateHeap
(0x1493B8) and drives random RtlAllocateHeap / RtlFreeHeap / RtlReAllocateHeap.
With `src/recomp_manual.c` linked in (`-DRT_HAVE_MANUAL -Ishim`), the same run checks the heap checker: `BADFREE_TEST=1` injects double and interior frees on the lifted side only, and the heap must stay identical to the original.

Result on this build: 50k+ ops over 12 seeds, zero divergence. The heap code is
lifted correctly.
