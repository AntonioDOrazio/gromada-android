"""Convert gromada-viewer C++ module units into plain headers for a unity build.

usage: demodularize.py <src_root> <dst_root>
"""
import re
import sys
from pathlib import Path

MODULE_TO_HEADER = {
    "utils": "core/utils.hpp",
    ":flags": "core/utils_flags.hpp",
    ":streamreader": "core/utils_streamreader.hpp",
    "cp866": "core/cp866.hpp",
    "engine.bounding_box": "core/bounding_box.hpp",
    "Gromada.Actions": "libgromada/actions.hpp",
    "Gromada.GameResources": "libgromada/game_resources.hpp",
    "Gromada.GraphicsDecoder": "libgromada/graphics_decoder.hpp",
    "Gromada.Map": "libgromada/map.hpp",
    "Gromada.ResourceReader": "libgromada/resource_reader.hpp",
    "Gromada.Resources": "libgromada/resources.hpp",
    "Gromada.Resources.Sound": "libgromada/resources_sound.hpp",
    "Gromada.SoftwareRenderer": "libgromada/software_renderer.hpp",
    "Gromada.Terromorphing": "libgromada/terramorphing.hpp",
    "Gromada.VisualLogic": "libgromada/visual_logic.hpp",
}

FILES = {
    "core/bounding_box.cppm": "core/bounding_box.hpp",
    "core/cp866.cppm": "core/cp866.hpp",
    "core/utils.cppm": "core/utils.hpp",
    "core/utils_flags.cppm": "core/utils_flags.hpp",
    "core/utils_streamreader.cppm": "core/utils_streamreader.hpp",
    "libgromada/actions.ixx": "libgromada/actions.hpp",
    "libgromada/game_resources.cppm": "libgromada/game_resources.hpp",
    "libgromada/graphics_decoder.cppm": "libgromada/graphics_decoder.hpp",
    "libgromada/map.cppm": "libgromada/map.hpp",
    "libgromada/map_loader.cpp": "libgromada/map_loader.inl",
    "libgromada/map_saver.cpp": "libgromada/map_saver.inl",
    "libgromada/resource_reader.cppm": "libgromada/resource_reader.hpp",
    "libgromada/resources.cppm": "libgromada/resources.hpp",
    "libgromada/resources_sound.cppm": "libgromada/resources_sound.hpp",
    "libgromada/software_renderer.cppm": "libgromada/software_renderer.hpp",
    "libgromada/terramorphing.cppm": "libgromada/terramorphing.hpp",
    "libgromada/visual_logic.cppm": "libgromada/visual_logic.hpp",
}


def strip_export_blocks(text: str) -> str:
    """Remove `export {` wrappers together with their matching closing brace."""
    out = []
    i = 0
    n = len(text)
    depth = 0
    export_depths = []  # brace depth at which an export block was opened
    while i < n:
        # skip comments and string/char literals verbatim
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j]); i = j; continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j]); i = j; continue
        if text[i] in "\"'":
            q = text[i]; j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1]); i = j + 1; continue
        m = re.match(r"export\s*\{", text[i:])
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            export_depths.append(depth)
            i += m.end(); continue
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            if export_depths and export_depths[-1] == depth:
                export_depths.pop(); i += 1; continue
            depth -= 1
        out.append(c); i += 1
    return "".join(out)


def convert(text: str, is_impl: bool) -> str:
    lines = []
    for line in text.splitlines():
        s = line.strip()
        if s == "module;":
            continue
        m = re.match(r"^export\s+module\s+([\w.:]+)\s*;", s)
        if m:
            lines.append("#pragma once")
            continue
        if re.match(r"^module\s+[\w.:]+\s*;", s):  # implementation unit
            continue
        if re.match(r"^(export\s+)?import\s+std\s*;", s):
            lines.append('#include "core/std.hpp"')
            continue
        m = re.match(r"^(export\s+)?import\s+([\w.:]+)\s*;", s)
        if m:
            lines.append(f'#include "{MODULE_TO_HEADER[m.group(2)]}"')
            continue
        lines.append(line)
    text = "\n".join(lines) + "\n"
    text = strip_export_blocks(text)
    # remaining `export` keywords in front of declarations
    text = re.sub(r"(?m)^(\s*)export\s+(?=(template|struct|class|enum|using|int|void|constexpr|std::|auto|inline|const|namespace|[A-Z]\w*\s))", r"\1", text)
    return text


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    for rel_src, rel_dst in FILES.items():
        text = (src / rel_src).read_text(encoding="utf-8")
        out = convert(text, rel_dst.endswith(".inl"))
        if re.search(r"\bexport\b", out.replace("ExportedFor", "")):
            print(f"warning: leftover 'export' in {rel_dst}")
        p = dst / rel_dst
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(out, encoding="utf-8")
        print("wrote", p)


if __name__ == "__main__":
    main()
