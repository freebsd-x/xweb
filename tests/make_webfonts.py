#!/usr/bin/env python3
"""Make the web fonts that tests/pages/webfont.html uses, from fonts installed here.

  python3 -I sdk/xweb/tests/make_webfonts.py outdir

A WOFF is a font's tables deflated. They're generated, not kept in the repository, because
the fonts aren't ours to carry around. There's no WOFF2 here: writing a valid one by hand
didn't work out, and decoding it is FreeType's job, so that's been checked by hand against
real fonts instead (see README).
"""
import os, struct, sys, zlib

SOURCES = ["/usr/local/share/fonts/dejavu", "/usr/share/fonts/dejavu", "/usr/share/fonts/truetype/dejavu",
           "/usr/local/share/fonts/urw-base35-fonts", "/usr/share/fonts/urw-base35"]


def source(name):
    for d in SOURCES:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit("no %s installed" % name)


def tables(ttf):
    flavor, count = struct.unpack(">IH", ttf[:6])
    out = []
    for i in range(count):
        tag, checksum, offset, length = struct.unpack(">4sIII", ttf[12 + 16 * i:28 + 16 * i])
        out.append((tag, checksum, ttf[offset:offset + length]))
    return flavor, out


def woff1(ttf):
    flavor, ts = tables(ttf)
    entries, blobs = [], []
    offset = 44 + 20 * len(ts)
    total_sfnt = 12 + 16 * len(ts)
    for tag, checksum, data in sorted(ts):
        packed = zlib.compress(data, 9)
        if len(packed) >= len(data):
            packed = data
        entries.append(struct.pack(">4sIIII", tag, offset, len(packed), len(data), checksum))
        padded = packed + b"\0" * (-len(packed) % 4)
        blobs.append(padded)
        offset += len(padded)
        total_sfnt += len(data) + (-len(data) % 4)
    header = struct.pack(">4sIIHHIHHIIIII", b"wOFF", flavor, offset, len(ts), 0, total_sfnt, 1, 0, 0, 0, 0, 0, 0)
    return header + b"".join(entries) + b"".join(blobs)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    made = {
        "serif.woff": woff1(open(source("DejaVuSerifCondensed.ttf"), "rb").read()),
        "mono-bold.woff": woff1(open(source("NimbusMonoPS-Bold.otf"), "rb").read()),
        "serif-italic.woff": woff1(open(source("DejaVuSerif-Italic.ttf"), "rb").read()),
        "sans.ttf": open(source("DejaVuSans-Bold.ttf"), "rb").read(),
    }
    for name, data in made.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


main()
