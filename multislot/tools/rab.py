"""EDF6 RAB archives ("SSA"): read and write, with the game's CMPL compression.

Layout, all little-endian unless noted (HUD/ONLINEHUDTEXTURE.RAB, 14 textures in one folder):
  0x00 'SSA\\0'   0x04 0x110   0x08 first data byte   0x0C largest stored entry   0x10 largest unpacked entry
  0x14 entry count   0x18 entry table   0x1C name index   0x20 folder count   0x24 folder table
  entry (0x20 bytes): name (UTF-16, offset from the entry), stored size, 16 zero bytes, data offset, folder
  name index (8 bytes each): name (offset from the pair), entry number; sorted by UTF-16 code unit
  folder (4 bytes each): name (offset from the folder entry)
  Entries are sorted by name ignoring case; the data of each entry follows the strings, unaligned.
Entry data is 'CMPL', the unpacked size (big-endian) and LZSS: a flag byte per eight items, bit set = literal
byte, bit clear = two bytes a, b copying (b & 15) + 3 bytes from ring position (a << 4) | (b >> 4) of a
4096-byte ring that starts zeroed with the write position at 0xFEE.
"""
import struct

_RING = 4096
_START = _RING - 18
_MIN = 3
_MAX = 18


def unpack(blob):
    if blob[:4] != b'CMPL':
        raise ValueError('not a CMPL entry')
    size = struct.unpack_from('>I', blob, 4)[0]
    ring = bytearray(_RING)
    r = _START
    out = bytearray()
    i = 8
    flags = 0
    while len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            flags = blob[i] | 0xFF00
            i += 1
        if flags & 1:
            source, length = None, 1
        else:
            a, b = blob[i], blob[i + 1]
            source, length = (a << 4) | (b >> 4), (b & 0x0F) + _MIN
        for k in range(length):
            # One byte at a time: a copy may overlap the bytes it is writing.
            c = blob[i] if source is None else ring[(source + k) & 0xFFF]
            out.append(c)
            ring[r] = c
            r = (r + 1) & 0xFFF
        i += 1 if source is None else 2
    if len(out) != size or i != len(blob):
        raise ValueError('CMPL entry does not end where its data does')
    return bytes(out)


def pack(data):
    """Greedy LZSS over the ring the reader keeps; matches only reach bytes already written to the ring."""
    out = bytearray(b'CMPL' + struct.pack('>I', len(data)))
    # Positions of every 3-byte prefix seen so far, newest last; the ring holds the last 4096 - 18 bytes safely.
    heads = {}
    window = _RING - _MAX
    i = 0
    while i < len(data):
        flag_at = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= len(data):
                break
            best_len, best_from = 0, 0
            key = bytes(data[i:i + _MIN])
            if len(key) == _MIN:
                for start in reversed(heads.get(key, ())):
                    if i - start > window:
                        break
                    length = _MIN
                    limit = min(_MAX, len(data) - i)
                    while length < limit and data[start + length] == data[i + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_from = length, start
                        if length == limit:
                            break
            if best_len >= _MIN:
                pos = (_START + best_from) & 0xFFF
                out += bytes((pos >> 4, ((pos & 0xF) << 4) | (best_len - _MIN)))
                step = best_len
            else:
                flags |= 1 << bit
                out.append(data[i])
                step = 1
            for k in range(i, i + step):
                if k + _MIN <= len(data):
                    heads.setdefault(bytes(data[k:k + _MIN]), []).append(k)
            i += step
        out[flag_at] = flags
    return bytes(out)


def _text(data, at):
    end = at
    while data[end:end + 2] != b'\0\0':
        end += 2
    return data[at:end].decode('utf-16-le')


def read(data):
    """Returns (folders, entries): folder names and (name, folder index, stored CMPL blob) in archive order."""
    if data[:4] != b'SSA\0':
        raise ValueError('not an SSA archive')
    count, table, _, folder_count, folder_table = struct.unpack_from('<5I', data, 0x14)
    folders = [_text(data, folder_table + 4 * i + struct.unpack_from('<I', data, folder_table + 4 * i)[0])
               for i in range(folder_count)]
    entries = []
    for i in range(count):
        at = table + 0x20 * i
        name, size, a, b, c, d, offset, folder = struct.unpack_from('<8I', data, at)
        if a or b or c or d:
            raise ValueError('entry fields this reader does not know are set')
        entries.append((_text(data, at + name), folder, data[offset:offset + size]))
    return folders, entries


def write(folders, entries):
    """Entries are (name, folder index, stored CMPL blob); they are written sorted the way the game's are."""
    entries = sorted(entries, key=lambda e: e[0].lower())
    order = sorted(range(len(entries)), key=lambda i: entries[i][0].encode('utf-16-be'))
    table = 0x28
    index = table + 0x20 * len(entries)
    folder_table = index + 8 * len(entries)
    strings = folder_table + 4 * len(folders)
    pool = bytearray()
    at = {}
    for text in folders + [e[0] for e in entries]:
        if text not in at:
            at[text] = strings + len(pool)
            pool += text.encode('utf-16-le') + b'\0\0'
    data_start = strings + len(pool)
    out = bytearray(data_start)
    struct.pack_into('<4sIIIIIIIII', out, 0, b'SSA\0', 0x110, data_start,
                     max(len(e[2]) for e in entries), max(struct.unpack_from('>I', e[2], 4)[0] for e in entries),
                     len(entries), table, index, len(folders), folder_table)
    offset = data_start
    for i, (name, folder, blob) in enumerate(entries):
        e = table + 0x20 * i
        struct.pack_into('<8I', out, e, at[name] - e, len(blob), 0, 0, 0, 0, offset, folder)
        offset += len(blob)
    for n, i in enumerate(order):
        p = index + 8 * n
        struct.pack_into('<II', out, p, at[entries[i][0]] - p, i)
    for i, name in enumerate(folders):
        f = folder_table + 4 * i
        struct.pack_into('<I', out, f, at[name] - f)
    out[strings:data_start] = pool
    for _, _, blob in entries:
        out += blob
    return bytes(out)
