#!/usr/bin/env python3
# the resource test's enumerations come from the same SDK headers that compile it
import re
import subprocess
import sys
from pathlib import Path

source = subprocess.run(sys.argv[2:] + ["-E", "-x", "c++", "-"],
                        input="#include <d3d11.h>\n#include <d3d10.h>\n", text=True,
                        capture_output=True, check=True).stdout


def entries(name):
    body = re.search(r"enum " + name + r"\s*\{(.*?)\}", source, re.S)
    assert body, name
    names = []
    for declaration in body[1].split(","):
        if not declaration.strip():
            continue
        item = re.fullmatch(r"\s*(\w+)(?:\s*=\s*.+)?\s*", declaration, re.S)
        assert item, declaration
        if not item[1].endswith("FORCE_UINT"):
            names.append(item[1])
    assert len(names) == len(set(names)), name
    return names


formats = entries("DXGI_FORMAT")
text = "struct ResourceFormat { DXGI_FORMAT value; const char *name; };\n"
text += "constexpr ResourceFormat resource_formats[] = {\n"
text += "".join(f'  {{{name}, "{name}"}},\n' for name in formats)
text += "};\n"
for enum, name in [("D3D11_BIND_FLAG", "resource_binds"), ("D3D10_RESOURCE_MISC_FLAG", "resource_misc10")]:
    text += f"constexpr UINT {name}[] = {{0, " + ", ".join(entries(enum)) + "};\n"
Path(sys.argv[1]).write_text(text)
