#!/usr/bin/env python3
"""Embed the 19 compressed Civ 7 LDN chat lines into the NRO."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINES = ROOT / "ldn_payloads" / "lines.txt"

def main() -> None:
    lines = [line for line in LINES.read_text(encoding="utf-8").splitlines() if line.strip()]
    out = ["#include \"ldn_data.h\"", "", "const char *ldn_chat_lines[] = {"]
    for line in lines:
        out.append("    " + json.dumps(line) + ",")
    out += ["};", "", "const size_t ldn_chat_line_count = sizeof(ldn_chat_lines) / sizeof(ldn_chat_lines[0]);", ""]
    (ROOT / "source" / "ldn_data.c").write_text("\n".join(out), encoding="utf-8")
    (ROOT / "include" / "ldn_data.h").write_text(
        "#pragma once\n#include <stddef.h>\n\nextern const char *ldn_chat_lines[];\nextern const size_t ldn_chat_line_count;\n",
        encoding="utf-8",
    )
    print(f"ldn_data: {len(lines)} lines")

if __name__ == "__main__":
    main()
