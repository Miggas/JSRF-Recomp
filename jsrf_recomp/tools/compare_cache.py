#!/usr/bin/env python3
"""Compare the game's hard-disk cache with the disc files it was copied from.

Jet Set Radio Future copies files from the disc (game\\Media\\...) to its cache
partition the first time it needs them, and from then on loads them from the
cache. Here the cache is the folder savegame\\Cache\\Media in the game folder.
A cached file that differs from the disc's copy -- written wrongly by an older
build, or cut short when the game was closed while copying -- is then loaded
every time, whatever the settings. This lists every cached file that does not
match the disc.

usage:  py -3 compare_cache.py [game folder]     (default: jsrf_recomp\\game)
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
game = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "game"))
cache = os.path.join(game, "savegame", "Cache", "Media")
disc = os.path.join(game, "Media")


def compare(c, d):
    """'same', 'padded' (the disc file plus extra bytes at the end) or
    'different' with the offset of the first byte that differs."""
    with open(c, "rb") as fc, open(d, "rb") as fd:
        off = 0
        while True:
            a = fd.read(1 << 20)
            b = fc.read(len(a)) if a else b""
            if not a:
                rest = fc.read(1)
                return ("same", None) if not rest else ("padded", None)
            if a != b:
                for i in range(min(len(a), len(b))):
                    if a[i] != b[i]:
                        return "different", off + i
                return "different", off + min(len(a), len(b))
            off += len(a)


def main():
    if not os.path.isdir(cache):
        print("No cache folder at %s -- nothing to compare (the game makes it as it goes)." % cache)
        return 0
    if not os.path.isdir(disc):
        print("No disc files at %s -- is this the game folder?" % disc)
        return 1
    same = diff = only = padded = 0
    pads = []
    for root, _, files in os.walk(cache):
        for f in files:
            c = os.path.join(root, f)
            rel = os.path.relpath(c, cache)
            d = os.path.join(disc, rel)
            if not os.path.isfile(d):
                # match case-insensitively, as the Xbox does
                parent = os.path.dirname(d)
                hit = None
                if os.path.isdir(parent):
                    for g in os.listdir(parent):
                        if g.lower() == os.path.basename(d).lower():
                            hit = os.path.join(parent, g)
                d = hit
            if not d:
                only += 1
                print("  only in the cache: %s (%d bytes)" % (rel, os.path.getsize(c)))
                continue
            sc, sd = os.path.getsize(c), os.path.getsize(d)
            how, at = compare(c, d)
            if how == "same":
                same += 1
            elif how == "padded":
                padded += 1
                pads.append(sc - sd)
            else:
                diff += 1
                print("  DIFFERENT: %s  (cache %d bytes, disc %d bytes; first difference at byte %d)"
                      % (rel, sc, sd, at))
    print("\n%d cached files match the disc exactly, %d match with extra bytes at the end,"
          " %d really differ, %d are only in the cache." % (same, padded, diff, only))
    if padded:
        print("Extra bytes at the end (%d to %d) are normal: the game copies whole disc"
              " sectors, so a cached file is rounded up to the next sector." % (min(pads), max(pads)))
    if diff:
        print("Some cached files do not match the disc. Send this list with your report:"
              " which files differ, and where, says what went wrong when they were copied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
