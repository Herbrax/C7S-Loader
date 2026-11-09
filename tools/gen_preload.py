#!/usr/bin/env python3
"""Embed the two default C7S payloads into the NRO for first-run preloading."""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = [
    ("c7s_preload_remote_js", "CIV7-RemoteJSLoader.C7S"),
    ("c7s_preload_reach_chat", "CIV7-Reach-Chat-Input.C7S"),
]


def array_lines(data: bytes) -> str:
    out = []
    for i in range(0, len(data), 20):
        out.append("    " + ",".join(str(b) for b in data[i:i + 20]) + ",")
    return "\n".join(out)


def main() -> None:
    header = [
        "#pragma once",
        "#include <stddef.h>",
        "",
    ]
    source = ['#include "preload_data.h"', ""]
    for symbol, name in FILES:
        data = (ROOT / "payloads" / name).read_bytes()
        header.append(f"extern const unsigned char {symbol}[];")
        header.append(f"extern const size_t {symbol}_len;")
        source.append(f"const unsigned char {symbol}[] = {{")
        source.append(array_lines(data))
        source.append("};")
        source.append(f"const size_t {symbol}_len = sizeof({symbol});")
        source.append("")
    (ROOT / "include" / "preload_data.h").write_text("\n".join(header) + "\n")
    (ROOT / "source" / "preload_data.c").write_text("\n".join(source) + "\n")
    print("preload_data generated")


if __name__ == "__main__":
    main()
