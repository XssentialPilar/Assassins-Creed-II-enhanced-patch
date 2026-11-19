"""Lay out the two states of the game's rendered PC prompts."""

import argparse
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


UNIT = 58
MAIN_ROWS = (
    (0x01, (0, 1), 0x3B, 0x3C, 0x3D, 0x3E, (0, .5), 0x3F, 0x40, 0x41, 0x42,
     (0, .5), 0x43, 0x44, 0x57, 0x58),
    (0x29, *range(0x02, 0x0E), (0x0E, 2)),
    ((0x0F, 1.5), *range(0x10, 0x1C), (0x2B, 1.5)),
    ((0x3A, 1.75), *range(0x1E, 0x29), (0x1C, 2.25)),
    ((0x2A, 2.25), *range(0x2C, 0x36), (0x36, 2.75)),
    ((0x1D, 1.25), (0xDB, 1.25), (0x38, 1.25), (0x39, 6.25),
     (0xB8, 1.25), (0xDC, 1.25), (0xDD, 1.25), (0x9D, 1.25)),
)
NAVIGATION = (
    (0xB7, 0x46, 0xC5), (0xD2, 0xC7, 0xC9), (0xD3, 0xCF, 0xD1),
    (), (0, 0xC8, 0), (0xCB, 0xD0, 0xCD),
)
NUMPAD = (
    (), (0x45, 0xB5, 0x37, 0x4A), (0x47, 0x48, 0x49, 0),
    (0x4B, 0x4C, 0x4D, 0), (0x4F, 0x50, 0x51, 0),
    ((0x52, 2), 0x53, 0),
)
EXTENDED = (
    (0x56, 0x64, 0x65, 0x66, 0x70, 0x73, 0x79, 0x7B, 0x7D, 0x7E, 0x8D, 0x91,
     0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0xB3),
    (0x90, 0x99, 0xA0, 0xA1, 0xA2, 0xA4, 0xAE, 0xB0, 0xB2, 0xDE, 0xDF, 0xE3, 0xE5,
     0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED),
)


def render(tiles: Path, output: Path) -> None:
    fonts = Path(os.environ["WINDIR"]) / "Fonts"
    heading = ImageFont.truetype(str(fonts / "seguisb.ttf"), 37)
    section = ImageFont.truetype(str(fonts / "seguisb.ttf"), 19)
    caption = ImageFont.truetype(str(fonts / "segoeui.ttf"), 15)
    canvas = Image.new("RGBA", (1378, 1396), "#e9e4dd")
    draw = ImageDraw.Draw(canvas)
    draw.text((36, 24), "Teclado y ratón", font=heading, fill="#252320")

    def put(code: int, x: float, y: float, family: int) -> None:
        if not code:
            return
        with Image.open(tiles / f"{family}-{code}.tga") as tile:
            glyph = tile.convert("RGBA")
        glyph = glyph.crop(glyph.getchannel("A").getbbox())
        glyph.thumbnail((52, 44), Image.Resampling.LANCZOS)
        canvas.alpha_composite(glyph, (round(x - glyph.width / 2), round(y - glyph.height / 2)))

    def row(values: tuple, left: int, y: int, family: int) -> None:
        x = float(left)
        for value in values:
            code, units = value if isinstance(value, tuple) else (value, 1)
            put(code, x + units * UNIT / 2, y, family)
            x += units * UNIT

    for family, top, state in ((20, 90, "En reposo"), (21, 747, "Al pulsar")):
        draw.rounded_rectangle((24, top, 1354, top + 625), radius=16, fill="#35322f")
        draw.text((46, top + 14), state, font=section, fill="#f0eae1")
        for index in range(6):
            y = top + 69 + index * 53
            row(MAIN_ROWS[index], 36, y, family)
            row(NAVIGATION[index], 922, y, family)
            row(NUMPAD[index], 1107, y, family)
        put(0x4E, 1107 + 3.5 * UNIT, top + 201.5, family)
        put(0x9C, 1107 + 3.5 * UNIT, top + 307.5, family)
        draw.line((46, top + 365, 1332, top + 365), fill="#686058", width=1)
        draw.text((46, top + 380), "Teclas adicionales", font=caption, fill="#e9e1d5")
        for index, codes in enumerate(EXTENDED):
            row(codes, 36, top + 429 + index * 51, family)
        draw.text((46, top + 513), "Ratón", font=caption, fill="#e9e1d5")
        row(tuple(range(256, 264)), 36, top + 564, family)
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(output, optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tiles", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    render(args.tiles, args.output)
