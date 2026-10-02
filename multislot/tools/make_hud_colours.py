"""Builds assets/ONLINEHUDTEXTURE.RAB: the game's online HUD textures with one colour per player for every
colour in src/hudcolours.h. Read-only on the game: the archive is read from Root.cpk and written into this
project only.

  python -B tools/make_hud_colours.py [out-path]

Only files are added; every file of the game's archive stays byte for byte, so the archive shows the game's own
HUD to a game without the plugin:
  player_lamps.dds  one 32x32 lamp per colour side by side (player_lamp.dds has four), the first four the game's
                    own pixels, the others painted from its red lamp; mipmaps down to 1x1 as the game's has
  chat_<name>.dds   a chat balloon for every colour past the game's four, painted from its red one
The plugin points the HUD at as many lamps and balloons as players (patches.cpp, HudColourPatches and
HudColourHooks) and writes the archive to Mods/HUD/ONLINEHUDTEXTURE.RAB while it is active (src/modfile.cpp);
EDFModLoader's redirector makes the game load it in place of the archived one.
When the output changes, add its size and FNV-1a 64 to kKnownHudArchives in src/modfile.cpp.
"""
import hashlib
import os
import pathlib
import re
import struct
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from cpk import Cpk  # noqa: E402
import crilayla  # noqa: E402
import rab  # noqa: E402

GAME = pathlib.Path(os.environ['EDF6_GAME_DIR']) if os.environ.get('EDF6_GAME_DIR') else HERE.parents[1]
# HUD/ONLINEHUDTEXTURE.RAB of the install this was made for (EDF.dll build 678CCB46).
ORIGINAL_SHA256 = '75a697cb2d84cdf2fd76c2178580805f54709b0fe9d60b3739f734ff70271330'
LAMP = 32  # one lamp is LAMP x LAMP pixels
TEMPLATE = 3  # the game's red: lamp 3 and chat_Red
LAMPS = 'player_lamps.dds'  # src/hud.cpp names it too


def palette():
    text = (HERE.parent / 'src' / 'hudcolours.h').read_text(encoding='utf-8')
    found = re.findall(r'\{L"(chat_\w+\.dds)", 0x([0-9A-Fa-f]{6})\}', text)
    if len(found) < 4 or [n for n, _ in found[:4]] != ['chat_Yellow.dds', 'chat_Green.dds', 'chat_Blue.dds', 'chat_Red.dds']:
        raise SystemExit('src/hudcolours.h does not start with the game\'s four colours')
    if len({n.lower() for n, _ in found}) != len(found):
        raise SystemExit('src/hudcolours.h names a chat texture twice')
    return [(name, np.array([int(rgb[i:i + 2], 16) for i in (0, 2, 4)], dtype=np.float64) / 255) for name, rgb in found]


def original_archive():
    archive = Cpk(os.path.join(GAME, 'Root.cpk'))
    entry = archive.index[('HUD', 'ONLINEHUDTEXTURE.RAB')]
    with open(archive.path, 'rb') as handle:
        handle.seek(archive.base + int(entry['FileOffset']))
        data = handle.read(int(entry['FileSize']))
    if int(entry['ExtractSize']) != int(entry['FileSize']):
        data = crilayla.decompress(data)
    return data


# The game's textures are uncompressed 32-bit DDS, but not in one byte order: player_lamp.dds stores R, G, B, A
# and the chat balloons B, G, R, A. The header's channel masks say which byte is which.
def channel_bytes(dds):
    masks = struct.unpack_from('<4I', dds, 92)  # R, G, B, A
    if struct.unpack_from('<I', dds, 88)[0] != 32 or sorted(masks) != [0xFF, 0xFF00, 0xFF0000, 0xFF000000]:
        raise SystemExit('a texture is not 8 bits per channel')
    return [mask.bit_length() // 8 - 1 for mask in masks]


def dds_pixels(dds):
    height, width = struct.unpack_from('<II', dds, 12)
    level0 = np.frombuffer(dds, np.uint8, width * height * 4, 128).reshape(height, width, 4)
    return level0[..., channel_bytes(dds)].astype(np.float64) / 255  # RGBA


def dds_bytes(header, levels):
    """`header` is a game texture's DDS header; size, pitch and mipmap count are set from `levels` (RGBA)."""
    height, width = levels[0].shape[:2]
    out = bytearray(header[:128])
    struct.pack_into('<III', out, 12, height, width, width * 4)
    if struct.unpack_from('<I', out, 28)[0]:  # the game's lamp has mipmaps; balloons have none
        struct.pack_into('<I', out, 28, len(levels))
    stored = np.argsort(channel_bytes(header))  # the RGBA channel each stored byte holds
    for level in levels:
        out += np.clip(np.rint(level[..., stored] * 255), 0, 255).astype(np.uint8).tobytes()
    return bytes(out)


def mipmaps(image):
    """Down to 1x1, averaging 2x2 blocks weighted by alpha so transparent pixels lend no colour."""
    levels = [image]
    while image.shape[0] > 1 or image.shape[1] > 1:
        h, w = max(1, image.shape[0] // 2), max(1, image.shape[1] // 2)
        fy, fx = image.shape[0] // h, image.shape[1] // w
        blocks = image.reshape(h, fy, w, fx, 4)
        alpha = blocks[..., 3:].sum(axis=(1, 3))
        colour = (blocks[..., :3] * blocks[..., 3:]).sum(axis=(1, 3)) / np.maximum(alpha, 1e-9)
        image = np.concatenate([colour, alpha / (fy * fx)], axis=-1)
        levels.append(image)
    return levels


def paint_lamp(red, colour):
    """The red lamp's red channel carries its shading and green/blue its highlight; both kept, hue replaced."""
    shade = red[..., 0]
    white = np.clip(red[..., 1:3].mean(axis=-1) / np.maximum(shade, 1e-9), 0, 1)
    rgb = shade[..., None] * (colour * (1 - white[..., None]) + white[..., None])
    return np.concatenate([np.clip(rgb, 0, 1), red[..., 3:]], axis=-1)


def paint_balloon(red, colour):
    """The red balloon is a red frame around grey: the frame's share of a pixel is how much redder than grey it is."""
    frame = np.clip((red[..., 0] - red[..., 1]) / (red[..., 0].max() - red[..., 1].min()), 0, 1)[..., None]
    grey = red[..., 1:2]
    tint = colour * red[..., 0:1] / red[..., 0].max()
    rgb = grey * (1 - frame) + tint * frame
    return np.concatenate([np.clip(rgb, 0, 1), red[..., 3:]], axis=-1)


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'assets' / 'ONLINEHUDTEXTURE.RAB'
    data = original_archive()
    if hashlib.sha256(data).hexdigest() != ORIGINAL_SHA256:
        raise SystemExit('Root.cpk has a different HUD/ONLINEHUDTEXTURE.RAB than the one these colours were made for')
    folders, entries = rab.read(data)
    files = {name: (folder, rab.unpack(blob)) for name, folder, blob in entries}
    colours = palette()

    lamp_header = files['player_lamp.dds'][1]
    lamps = dds_pixels(lamp_header)
    if lamps.shape != (LAMP, 4 * LAMP, 4):
        raise SystemExit(f'player_lamp.dds is {lamps.shape}, not four {LAMP}x{LAMP} lamps')
    red_lamp = lamps[:, TEMPLATE * LAMP:(TEMPLATE + 1) * LAMP]
    row = [lamps[:, i * LAMP:(i + 1) * LAMP] if i < 4 else paint_lamp(red_lamp, rgb) for i, (_, rgb) in enumerate(colours)]
    lamp = dds_bytes(lamp_header, mipmaps(np.concatenate(row, axis=1)))
    # The game's own four keep their exact level-0 pixels.
    if not np.array_equal(dds_pixels(lamp)[:, :4 * LAMP], lamps):
        raise SystemExit("the first four lamps are not the game's")

    folder, red_chat = files['chat_Red.dds']
    balloon = dds_pixels(red_chat)
    added = {LAMPS: lamp}
    for name, rgb in colours[4:]:
        added[name] = dds_bytes(red_chat, [paint_balloon(balloon, rgb)])
    for name in added:
        if name in files:
            raise SystemExit(f"{name} is already in the game's archive")
    archive = rab.write(folders, entries + [(name, folder, rab.pack(content)) for name, content in added.items()])

    # What was added unpacks to what was put in, and the game's own files are stored exactly as the game has them.
    check_folders, check = rab.read(archive)
    unpacked = {name: rab.unpack(blob) for name, _, blob in check}
    stored = {name: blob for name, _, blob in check}
    if check_folders != folders or any(unpacked[name] != content for name, content in added.items()):
        raise SystemExit('the archive does not read back')
    for name, _, blob in entries:
        if stored[name] != blob:
            raise SystemExit(f'{name} changed')
    if sorted(unpacked) != sorted(set(files) | set(added)):
        raise SystemExit("the archive does not hold the game's files plus the lamps and one balloon per colour")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(archive)
    print(f'original {len(data)} bytes sha256 {hashlib.sha256(data).hexdigest()}')
    fnv = 0xCBF29CE484222325
    for byte in archive:
        fnv = ((fnv ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    print(f'wrote {out} ({len(archive)} bytes, {len(colours)} colours, sha256 {hashlib.sha256(archive).hexdigest()}, '
          f'fnv1a64 {fnv:#018x})')


if __name__ == '__main__':
    main()
