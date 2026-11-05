#!/usr/bin/env python3
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [
    ("index.html", "web_index_html"),
    ("app.css", "web_app_css"),
    ("app.js", "web_app_js"),
]


def emit(name, data):
    out = ["static const unsigned char %s[] = {" % name]
    line = []
    for i, b in enumerate(data):
        line.append("0x%02x," % b)
        if len(line) == 20:
            out.append("    " + "".join(line))
            line = []
    if line:
        out.append("    " + "".join(line))
    out.append("};")
    out.append("static const unsigned int %s_len = (unsigned int)sizeof(%s);" % (name, name))
    out.append("")
    return "\n".join(out)


def main():
    header = [
        "#pragma once",
        "",
    ]
    for fname, cname in FILES:
        path = os.path.join(ROOT, "web", fname)
        with open(path, "rb") as f:
            data = f.read()
        header.append(emit(cname, data))
    text = "\n".join(header)
    outpath = os.path.join(ROOT, "include", "web_assets.h")
    old = None
    if os.path.exists(outpath):
        with open(outpath, "r", encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(outpath, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("web_assets.h updated")
    else:
        print("web_assets.h unchanged")


if __name__ == "__main__":
    main()
