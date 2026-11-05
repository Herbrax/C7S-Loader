#!/usr/bin/env python3
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = [
    ("NotoSans-Regular.ttf", "font_regular_ttf"),
    ("NotoSans-SemiBold.ttf", "font_bold_ttf"),
    ("IBMPlexMono-Medium.ttf", "font_mono_ttf"),
]


def emit(name, data):
    out = ["const unsigned char %s[] = {" % name]
    line = []
    for b in data:
        line.append("0x%02x," % b)
        if len(line) == 24:
            out.append("    " + "".join(line))
            line = []
    if line:
        out.append("    " + "".join(line))
    out.append("};")
    out.append("const unsigned long %s_size = (unsigned long)sizeof(%s);" % (name, name))
    out.append("")
    return "\n".join(out)


def main():
    parts = ["#include <stddef.h>", ""]
    for fname, cname in FONTS:
        with open(os.path.join(ROOT, "assets", "fonts", fname), "rb") as f:
            parts.append(emit(cname, f.read()))
    text = "\n".join(parts)
    outpath = os.path.join(ROOT, "source", "fonts_data.c")
    old = None
    if os.path.exists(outpath):
        with open(outpath, "r", encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(outpath, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("fonts_data.c updated")
    else:
        print("fonts_data.c unchanged")


if __name__ == "__main__":
    main()
