"""Compose the sprites exported by ac2_controller_smoke --render-atlas."""
import argparse
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def render(tiles: Path, output: Path, version: str) -> None:
    fonts = Path(os.environ["WINDIR"]) / "Fonts"
    def font(name, size):
        return ImageFont.truetype(str(fonts / name), size)

    canvas = Image.new("RGBA", (1440, 1236), "#f2efeb")
    draw = ImageDraw.Draw(canvas)
    draw.rectangle((0, 0, 1440, 8), fill="#9b283e")
    draw.text((42, 38), "Input prompts", font=font("segoeuil.ttf", 48), fill="#252229")
    draw.text((43, 107), "Assassin’s Creed II Enhanced Patch", font=font("seguisb.ttf", 20), fill="#95273a")
    draw.text((1397, 116), f"Version {version}", font=font("seguisb.ttf", 16), anchor="rs", fill="#544f58")
    labels = ("FEET", "EMPTY HAND", "WEAPON HAND", "HEAD", "LOCK ON", "HIGH PROFILE",
              "CONTEXT CAMERA", "WEAPON WHEEL", "PAUSE", "MAP")
    families = ((0, "PC · released"), (5, "PC · pressed"),
                (2, "DualShock 3 · released"), (8, "DualShock 3 · pressed"),
                (3, "DualShock 4 · released"), (9, "DualShock 4 · pressed"),
                (1, "Xbox · released"), (7, "Xbox · pressed"),
                (4, "DualSense · released"), (10, "DualSense · pressed"))
    role_font = font("seguisb.ttf", 11)
    for index, label in enumerate(labels):
        draw.text((253 + index * 113, 173), label, font=role_font, anchor="mm", fill="#5b5260")
    for row, (family_id, name) in enumerate(families):
        y = 200 + row * 96
        draw.rounded_rectangle((34, y, 1406, y + 82), radius=8,
                               fill="#ddd8d2" if row < 2 else "#e8e3de", outline="#d1c7c1")
        draw.text((51, y + 30), name, font=font("seguisb.ttf", 16), fill="#27232a")
        for index in range(len(labels)):
            tile = Image.open(tiles / f"{family_id}-{index + 1}.tga").convert("RGBA")
            enlarged = tile.resize((64, 64), Image.Resampling.LANCZOS)
            canvas.alpha_composite(enlarged, (221 + index * 113, y + 9))
    draw.text((44, 1179), "Sprites rendered by the patch. The PC rows use the default keyboard and mouse bindings.",
              font=font("segoeui.ttf", 16), fill="#544f58")
    draw.text((44, 1202), "In-game prompts update as you change your bindings in Controls.",
              font=font("segoeui.ttf", 15), fill="#544f58")
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(output, optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tiles", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--version", required=True)
    options = parser.parse_args()
    render(options.tiles, options.output, options.version)
