# the SDK enumerates the formats; their names give component widths and type groups (D3D11.3 19.1.3.1, 19.1.4).
# BC blocks are 64 bits for BC1/BC4, 128 for the rest (Microsoft Learn, Texture Block Compression in Direct3D 11).
import re
import subprocess
import sys

text = subprocess.run(sys.argv[1:] + ['-E', '-P', '-x', 'c++', '-'], input='#include <dxgiformat.h>\n',
                      capture_output=True, text=True, check=True).stdout
body = re.search(r'typedef enum DXGI_FORMAT\s*\{(.*?)\}\s*DXGI_FORMAT', text, re.S)
assert body, 'DXGI_FORMAT was not found in the SDK'
print('struct CopyFormat { DXGI_FORMAT id; const char *name, *family; unsigned bytes, unit, defined; bool depth; };')
print('static const CopyFormat copy_formats[] = {')
formats = []
names = re.findall(r'\bDXGI_FORMAT_(\w+)\s*=', body[1])
groups = {name[:-len('_TYPELESS')] for name in names if re.fullmatch(r'(?:[RGBAX]\d+)+_TYPELESS', name)}
for name in names:
    layout = re.sub(r'_(FLOAT|UNORM|TYPELESS)_(?=[RGBAXDS]\d)', '', name)
    match = re.fullmatch(r'((?:[RGBAXEDS]\d+)+|BC\d+H?)_'
                         r'(TYPELESS|FLOAT|UF16|SF16|UINT|SINT|UNORM(?:_SRGB)?|SNORM|SHAREDEXP)',
                         layout)
    if not match:
        continue
    family = match[1].replace('D', 'R').replace('S', 'G')
    defined = 0
    if family.startswith('BC'):
        assert re.fullmatch(r'BC(?:[1-5]|6H|7)', family), f'unknown block layout: {name}'
        size, unit = (8 if family in ('BC1', 'BC4') else 16), 4
        defined = (1 << size) - 1
    else:
        bits = 0
        for component, width in re.findall(r'([RGBAXEDS])(\d+)', match[1]):
            width = int(width)
            if component != 'X':
                defined |= ((1 << width) - 1) << bits
            bits += width
        if bits % 8:
            continue
        defined = sum(1 << byte for byte in range(bits // 8) if defined >> (byte * 8) & 255)
        size, unit = bits // 8, 1
    resource = name
    if layout != name and not name.startswith('D'):
        # depth/stencil planes use their type group's texture storage (D3D11.3 7.18.15.1).
        parents = [group for group in groups if re.fullmatch(family.replace('X', '[RGBAX]'), group)]
        assert len(parents) == 1, f'no unique resource layout for {name}: {parents}'
        family = parents[0]
        resource = family + '_TYPELESS'
    formats.append((size, name, family, unit, defined, resource))
required = {name for name in names if re.match(r'D\d', name) or name.endswith('_SRGB')}
assert required <= {format[1] for format in formats}, 'depth or sRGB formats were omitted'
for size, name, family, unit, defined, resource in sorted(formats):
    depth = name.startswith('D') or resource != name
    label = name if resource == name else f'{name} via {resource}'
    print(f'  {{DXGI_FORMAT_{resource}, "{label}", "{family}", {size}, {unit}, {defined}, {str(depth).lower()}}},')
print('};')
