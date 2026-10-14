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


def digest(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if not os.path.isdir(cache):
        print("No cache folder at %s -- nothing to compare (the game makes it as it goes)." % cache)
        return 0
    if not os.path.isdir(disc):
        print("No disc files at %s -- is this the game folder?" % disc)
        return 1
    same = diff = only = 0
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
            if sc != sd or digest(c) != digest(d):
                diff += 1
                print("  DIFFERENT: %s  (cache %d bytes, disc %d bytes)" % (rel, sc, sd))
            else:
                same += 1
    print("\n%d cached files match the disc, %d differ, %d are only in the cache." % (same, diff, only))
    if diff:
        print("The cache is damaged. Close the game, delete the folder\n  %s\n"
              "(your saves are elsewhere and are kept) and play again: the game\n"
              "copies the files from the disc again." % os.path.join(game, "savegame", "Cache"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
