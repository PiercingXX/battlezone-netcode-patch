#!/usr/bin/env python3
"""Assert the C++ BufferLogRecordHeader matches the decoder's struct format.

Three files have to agree on one binary layout: the record header in each
proxy, and `HDR = struct.Struct(...)` in decode_buffer_log.py. Nothing pinned
them together, so a field added to the C++ side would have silently shifted
every subsequent field in the decoder — and the failure mode is not a crash, it
is plausible-looking wrong numbers, which this project has already spent two
releases chasing in the sequence field.

This parses all three and compares them field by field. It needs no compiler
and no game, so CI runs it on every push.

Exit status 0 when all three agree, 1 otherwise.
"""
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CPP_SOURCES = (
    'Linux/proton_dsound_proxy/src/dsound_proxy.cpp',
    'Microslop/winmm_proxy/src/netcode_hooks.cpp',
)
DECODER = 'buffer-logging/decode_buffer_log.py'

STRUCT_RE = re.compile(
    r'struct\s+BufferLogRecordHeader\s*\{(.*?)\};', re.S)
FIELD_RE = re.compile(r'\b(uint8_t|uint16_t|uint32_t|uint64_t)\s+(\w+)\s*;')
HDR_RE = re.compile(r"^HDR\s*=\s*struct\.Struct\('(<[^']+)'\)", re.M)

# struct-module code -> (C type it must correspond to, size in bytes)
CODE_TO_C = {
    'B': ('uint8_t', 1),
    'H': ('uint16_t', 2),
    'I': ('uint32_t', 4),
    'Q': ('uint64_t', 8),
}


def parse_cpp(path):
    with open(path) as fh:
        text = fh.read()
    m = STRUCT_RE.search(text)
    if not m:
        return None, False
    # The struct must sit inside an open #pragma pack(push, 1) region. With
    # the current field order every member is naturally aligned, so dropping
    # the pragma keeps every offset identical but grows sizeof() with trailing
    # padding — the stride changes, the decoder mis-slices payloads, and the
    # field-by-field comparison below would still pass.
    before = text[:m.start()]
    last_push = before.rfind('#pragma pack(push, 1)')
    last_pop = before.rfind('#pragma pack(pop)')
    packed = last_push != -1 and last_push > last_pop
    return FIELD_RE.findall(m.group(1)), packed


def parse_decoder(path):
    with open(path) as fh:
        text = fh.read()
    m = HDR_RE.search(text)
    if not m:
        return None
    return m.group(1)


def expand(fmt):
    """'<IIIIQ' -> ['I','I','I','I','Q'], honouring repeat counts."""
    out = []
    count = ''
    for ch in fmt:
        if ch in '<>=!@':
            continue
        if ch.isdigit():
            count += ch
            continue
        out.extend([ch] * (int(count) if count else 1))
        count = ''
    return out


def main():
    ok = True

    layouts = {}
    for rel in CPP_SOURCES:
        fields, packed = parse_cpp(os.path.join(REPO, rel))
        if fields is None:
            print(f'FAIL {rel}: no BufferLogRecordHeader found')
            ok = False
            continue
        if not packed:
            print(f'FAIL {rel}: BufferLogRecordHeader is not inside an open '
                  f'#pragma pack(push, 1) region — sizeof() gains trailing '
                  f'padding and the write stride no longer matches the decoder')
            ok = False
        layouts[rel] = fields

    # The two proxies must agree with each other first: they write the same
    # file format, and a capture is decoded without knowing which produced it.
    names = list(layouts)
    if len(names) == 2 and layouts[names[0]] != layouts[names[1]]:
        print('FAIL the two proxies disagree on the record header:')
        for a, b in zip(layouts[names[0]], layouts[names[1]]):
            mark = '  ' if a == b else '<-'
            print(f'  {mark} {a} | {b}')
        return 1
    if not layouts:
        return 1

    cpp = layouts[names[0]]

    fmt = parse_decoder(os.path.join(REPO, DECODER))
    if fmt is None:
        print(f'FAIL {DECODER}: no `HDR = struct.Struct(...)` found')
        return 1
    if not fmt.startswith('<'):
        print(f'FAIL {DECODER}: format is {fmt!r}; it must be little-endian '
              f'and unpadded, i.e. start with "<"')
        ok = False

    codes = expand(fmt)
    if len(codes) != len(cpp):
        print(f'FAIL field count: C++ has {len(cpp)}, decoder has {len(codes)}')
        for i in range(max(len(cpp), len(codes))):
            c = f'{cpp[i][0]} {cpp[i][1]}' if i < len(cpp) else '(missing)'
            d = codes[i] if i < len(codes) else '(missing)'
            print(f'  [{i}] {c:<28} {d}')
        return 1

    offset = 0
    for i, (code, (ctype, cname)) in enumerate(zip(codes, cpp)):
        want = CODE_TO_C.get(code)
        if want is None:
            print(f'FAIL [{i}] {cname}: decoder uses unsupported code {code!r}')
            ok = False
            continue
        if want[0] != ctype:
            print(f'FAIL [{i}] offset {offset}: C++ has {ctype} {cname}, '
                  f'decoder has {code} ({want[0]})')
            ok = False
        offset += want[1]

    size = struct.calcsize(fmt)
    if size != offset:
        print(f'FAIL total size: fields sum to {offset}, struct.calcsize says '
              f'{size} — the format is padding, which #pragma pack(1) is not')
        ok = False

    if ok:
        print(f'ok: {len(cpp)} fields, {size} bytes, all three sources agree')
        print(f'    {" ".join(n for _t, n in cpp)}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
