"""Writes the seed inputs of CandidateWindow_fuzz (test/fuzz/seeds/).

    python make_seeds.py

Each seed is a program for CandidateWindow_fuzz.cpp: two bytes for the font
(height, face), then operations, one byte each followed by their arguments, in
the encoding of the Input class there. The seeds are the windows 大易, 酷倉 and
新酷音 really show, plus the extreme settings a config.json can ask for.
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SEEDS = os.path.join(HERE, "seeds")

# operations (CandidateWindow_fuzz.cpp: switch (in.byte() % 24))
ADD, HEADER, PAGE_INFO, MESSAGE, TEXT_ROWS, SET_ITEMS = 0, 3, 4, 5, 6, 7
PER_ROW, SPACING, KEY_STYLE, MESSAGE_STYLE, HEADER_STYLE = 8, 9, 10, 11, 12
STABLE_WIDTH, MAX_WIDTH, USE_CURSOR, CURRENT_SEL, KEY, CLEAR = 13, 14, 15, 16, 17, 18
HIGH_CONTRAST, THEME, SEED_STABLE, RESET_STABLE, PAINT = 19, 20, 21, 22, 23
UP, DOWN, LEFT, RIGHT, RETURN = 0, 1, 2, 3, 4
JHENGHEI, SEGOE, EMOJI, MISSING = 0, 1, 2, 3


def i32(value):
    """Input::i32, kind 3: any 32-bit value"""
    return bytes([3]) + struct.pack("<i", value)


def wchar(c):
    return struct.pack("<H", ord(c) if isinstance(c, str) else c)


def wstr(text):
    """Input::wstr: length, then (kind, unit) pairs; kind 0 = the UTF-16 unit given"""
    if isinstance(text, str):
        encoded = text.encode("utf-16-le")
        units = [encoded[i:i + 2] for i in range(0, len(encoded), 2)]
    else:
        units = text
    assert len(units) < 48
    return bytes([len(units)]) + b"".join(b"\x00" + u for u in units)


def raw_units(*units):
    return [struct.pack("<H", u) for u in units]


def font(height=16, face=JHENGHEI):
    return bytes([height, face])


def add(item, key):
    return bytes([ADD]) + wstr(item) + wchar(key)


def op(code, *args):
    return bytes([code]) + b"".join(args)


def flag(on):
    return bytes([1 if on else 0])


def write(name, data):
    os.makedirs(SEEDS, exist_ok=True)
    with open(os.path.join(SEEDS, name), "wb") as f:
        f.write(data)


def main():
    dayi = (font() + op(PER_ROW, i32(6)) + b"".join(add(c, k) for c, k in zip("人入八乂亼仌", "123456"))
            + op(HEADER, wstr("大易 人")) + op(PAGE_INFO, wstr("1/1")) + op(CURRENT_SEL, i32(0)) + op(PAINT)
            + op(KEY, bytes([RIGHT])) + op(KEY, bytes([DOWN])) + op(PAINT))
    write("dayi_row", dayi)

    chewing = (font(20) + op(PER_ROW, i32(1)) + b"".join(add(c, k) for c, k in zip("你妳擬尼泥逆膩倪匿", "123456789"))
               + op(PAGE_INFO, wstr("1/3")) + op(CURRENT_SEL, i32(4)) + op(KEY_STYLE, i32(3)) + op(PAINT)
               + op(KEY, bytes([UP])) + op(KEY, bytes([RETURN])) + op(PAINT))
    write("chewing_column", chewing)

    message = (font() + op(MESSAGE, wstr("正在載入輸入法碼表")) + op(MESSAGE_STYLE, i32(2)) + op(PAINT)
               + op(TEXT_ROWS, wstr("加入：你好"), wstr("新酷音"), wstr("")) + op(PAINT) + op(CLEAR) + op(PAINT))
    write("message_only", message)

    items = op(SET_ITEMS, bytes([5, 3]), wstr("日"), wstr("曰"), wstr("𠀀"), wstr(""), wstr("ab"), wchar("1"), wchar("2"), wchar("3"))
    write("set_items_mismatched_keys", font() + items + op(PER_ROW, i32(2)) + op(PAINT))

    extremes = font(0, MISSING) + b"".join(add(c, k) for c, k in zip("一二三四", "1234"))
    for value in (0, -1, -2147483648, 2147483647, 100000):
        extremes += op(PER_ROW, i32(value)) + op(PAINT)
        extremes += op(SPACING, i32(value), i32(value), i32(value)) + op(PAINT)
        extremes += op(MAX_WIDTH, flag(True), i32(value)) + op(PAINT)
        extremes += op(STABLE_WIDTH, flag(True), i32(value)) + op(SEED_STABLE, i32(value)) + op(PAINT)
        extremes += op(CURRENT_SEL, i32(value)) + op(KEY, bytes([LEFT])) + op(PAINT)
    write("extreme_numbers", extremes)

    styles = font(18, SEGOE) + b"".join(add(c, k) for c, k in zip("甲乙丙", "abc")) + op(HEADER, wstr("酷倉 日"))
    for value in list(range(-1, 18)) + [255, -2147483648]:
        styles += op(KEY_STYLE, i32(value)) + op(MESSAGE_STYLE, i32(value)) + op(HEADER_STYLE, i32(value)) + op(PAINT)
    styles += op(HIGH_CONTRAST, flag(True)) + op(PAINT) + op(HIGH_CONTRAST, flag(False))
    styles += op(THEME, bytes(range(21))) + op(USE_CURSOR, flag(False)) + op(PAINT)
    write("all_styles", styles)

    strings = font(16, EMOJI)
    strings += add(raw_units(0xD83D, 0xDE00), 0xD800)             # emoji, a lone surrogate as the key
    strings += add(raw_units(0xDC00, 0xD800), 0)                  # swapped surrogates, NUL key
    strings += add(raw_units(0x0041, 0x0300, 0x0301, 0x0302), 9)  # combining marks, TAB key
    strings += add(raw_units(0x05D0, 0x05D1, 0x200F, 0x0627), 0xFFFF)  # RTL
    strings += add(raw_units(0x0000, 0x000A, 0x000D, 0x0009), 0x20)
    strings += add("x" * 47, "1")
    strings += op(HEADER, wstr(raw_units(*([0xD800] * 47)))) + op(PAGE_INFO, wstr(raw_units(0xFEFF, 0x202E)))
    strings += op(PER_ROW, i32(3)) + op(PAINT) + op(MAX_WIDTH, flag(True), i32(40)) + op(PAINT)
    write("odd_strings", strings)

    many = font(12) + op(PER_ROW, i32(10)) + b"".join(add("候選" + str(i), chr(0x30 + i % 10)) for i in range(60))
    many += op(CURRENT_SEL, i32(59)) + op(PAINT) + b"".join(op(KEY, bytes([d])) for d in (DOWN, DOWN, RIGHT, UP, LEFT)) + op(PAINT)
    write("many_candidates", many)

    for name in sorted(os.listdir(SEEDS)):
        print("%-28s %5d bytes" % (name, os.path.getsize(os.path.join(SEEDS, name))))


if __name__ == "__main__":
    main()
