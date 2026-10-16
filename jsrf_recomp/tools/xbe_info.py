#!/usr/bin/env python3
"""Show what a default.xbe is, and whether its code matches the one the
project's analysis was made from.

usage:  py -3 tools/xbe_info.py [path/to/default.xbe]

The check fingerprints the sections the game loads into memory (its code and
data), not the whole file. The XBE header -- region, allowed media, the
signature -- is often patched by dumping and repacking tools (to run from a
hard drive, or from any region), which changes the whole file's SHA-256 but
not a byte of the game's code. Those copies work; only a different code
revision (PAL, NTSC-J, a later reprint) does not.
"""
import datetime
import hashlib
import os
import struct
import sys

# The sections of the NTSC retail default.xbe the analysis was made from:
# name -> SHA-256 of its raw data in the file. The two $$X..IMAGE sections are
# the dashboard's title and save icons, not loaded with the game: shown, but
# they do not need to match.
EXPECTED_SECTIONS = {
    ".text":     "a4fe6974b2ccda59b313cde582a0b27858a923f7585661ab27e5e49b3a0d0ffa",
    "D3D":       "2fb19dfa45ad25275967f0076773254aa74c20486703f361a24fc80ef145329a",
    "DSOUND":    "24b8aacf7c50c207084e28c8b88549b530c6a199655dc7e14461b42758f6cadd",
    "MMATRIX":   "25b0ce0f7c0f8397c4cb818fd52f5aaaad25ce8430acc2a42f0116278b4ae05c",
    "XGRPH":     "a3a8a328d258f61f825478053e7d83e4978ba61ebfd43d03c807d70e9967fb95",
    "XPP":       "ac8f4321e9fd4dc9b5479d64ed32079a821649102b01d61f3f6e97191e3b61da",
    ".rdata":    "508b2c93e12bd1d05a668c41389a29ac8e6d4a9e8efefcc8e47f9f78b03cfd7d",
    ".data":     "8ce0236715a22c04420388534896dfcb64c54ca852554ab52777504cf960fcf8",
    "DOLBY":     "c493eb6f8390ec48cde2da2995c114662059f7790f99d68adbda76a63b4b46ae",
}
OPTIONAL_SECTIONS = ("$$XTIMAGE", "$$XSIMAGE")
# Whole-file SHA-256 of the copy the project was developed with. Its header
# is patched (region 7, all media), so a clean dump differs here: that is fine.
DEV_FILE_SHA256 = "bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d"

REGIONS = {1: "North America", 2: "Japan", 4: "Rest of world (PAL)", 0x80000000: "Manufacturing"}


class XbeError(Exception):
    pass


def read_xbe(path):
    """Header facts and the sections, as a dict."""
    d = open(path, "rb").read()
    if d[:4] != b"XBEH":
        raise XbeError("%s is not an XBE file" % path)
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    base = u32(0x104)
    off = lambda va: va - base

    def cstr(va, n=32):
        o = off(va)
        return d[o:o + n].split(b"\0")[0].decode("latin-1")

    info = {"size": len(d), "file_sha256": hashlib.sha256(d).hexdigest()}
    c = off(u32(0x118))
    info["cert_time"] = u32(c + 4)
    info["title_id"] = u32(c + 8)
    info["title"] = d[c + 0xC:c + 0xC + 80].decode("utf-16le", "replace").split("\0")[0]
    info["media"] = u32(c + 0x9C)
    info["region"] = u32(c + 0xA0)
    info["disk"] = u32(c + 0xA8)
    info["version"] = u32(c + 0xAC)
    nsec, secs = u32(0x11C), off(u32(0x120))
    sections = []
    for i in range(nsec):
        s = secs + i * 56
        raw_addr, raw_size, name_va = u32(s + 12), u32(s + 16), u32(s + 20)
        sections.append((cstr(name_va), d[raw_addr:raw_addr + raw_size]))
    info["sections"] = sections
    return info


def check(path):
    """(ok, info, per-section results). ok: the code is the right one."""
    info = read_xbe(path)
    results = []
    for name, data in info["sections"]:
        got = hashlib.sha256(data).hexdigest()
        if name in OPTIONAL_SECTIONS:
            state = "(icon)"
        elif EXPECTED_SECTIONS.get(name) == got:
            state = "ok"
        else:
            state = "DIFFERENT"
        results.append((name, len(data), got, state))
    code = [(r[0], r[3]) for r in results if r[0] not in OPTIONAL_SECTIONS]
    ok = (info["file_sha256"] == DEV_FILE_SHA256 or
          ([n for n, _ in code] == list(EXPECTED_SECTIONS) and
           all(st == "ok" for _, st in code)))
    return ok, info, results


def describe(info, results):
    regions = [v for k, v in REGIONS.items() if info["region"] & k] or ["none"]
    lines = [
        "  title       %s (title ID %08X, version %d, disc %d)" % (
            info["title"], info["title_id"], info["version"], info["disk"]),
        "  built       %s" % datetime.datetime.fromtimestamp(
            info["cert_time"], datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "  region      %s (0x%X)" % (", ".join(regions), info["region"]),
        "  media       0x%08X" % info["media"],
        "  file        %d bytes, SHA-256 %s" % (info["size"], info["file_sha256"]),
        "  sections:",
    ]
    for name, size, got, state in results:
        lines.append("    %-10s %7X  %s  %s" % (name, size, got[:16], state))
    return "\n".join(lines)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "game", "default.xbe")
    try:
        ok, info, results = check(path)
    except (OSError, XbeError, struct.error) as e:
        print("ERROR: %s" % e)
        return 1
    print(describe(info, results))
    if ok:
        print("\nThe game's code matches: this copy works with the project.")
        if info["file_sha256"] != DEV_FILE_SHA256:
            print("(Only the header differs from the developer's copy -- that is normal.)")
        return 0
    print("\nThe game's code is DIFFERENT from the version the project is made for "
          "(the NTSC retail release). Please share the lines above.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
