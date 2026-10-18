#!/usr/bin/env python3
"""Extracts the first icon group (RT_GROUP_ICON) of a Windows exe into a .ico file.
usage: exe_icon.py <game.exe> <out.ico>   (needs pefile, see build.sh)"""
import struct
import sys

import pefile

RT_ICON, RT_GROUP_ICON = 3, 14


def first_data(entry):
    """Data of the first leaf below a resource directory entry (first name, first language)."""
    while hasattr(entry, 'directory'):
        entry = entry.directory.entries[0]
    return entry.data.struct


def main(exe, out):
    pe = pefile.PE(exe)
    res = {e.id: e for e in pe.DIRECTORY_ENTRY_RESOURCE.entries}
    g = first_data(res[RT_GROUP_ICON])
    group = pe.get_data(g.OffsetToData, g.Size)
    count = struct.unpack_from('<H', group, 4)[0]
    icons = {e.id: e for e in res[RT_ICON].directory.entries}
    entries, body = b'', b''
    offset = 6 + 16 * count
    for i in range(count):
        w, h, colors, _, planes, bpp, _, icon_id = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i)
        d = first_data(icons[icon_id])
        data = pe.get_data(d.OffsetToData, d.Size)
        entries += struct.pack('<BBBBHHII', w, h, colors, 0, planes, bpp, len(data), offset + len(body))
        body += data
    with open(out, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, count) + entries + body)


if __name__ == '__main__':
    main(*sys.argv[1:3])
