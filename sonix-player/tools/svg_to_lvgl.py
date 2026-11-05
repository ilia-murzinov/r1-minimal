#!/usr/bin/env python3
"""
Rasterises the SVG icons in assets/icons/ into a C source file LVGL can draw
(src/gui/shell/icons.c + icons.h).

The generated file is checked into the repo, so building the player does NOT
need Python, cairosvg or any of this. Re-run it only when an icon changes or a
new one is added:

    pip install cairosvg pillow
    python3 tools/svg_to_lvgl.py

Two kinds of icon come out of this:

  * SVGs are emitted as ARGB8888 *white* shapes. The colour comes from LVGL at
    draw time via `lv_obj_set_style_image_recolor()`, so the same bitmap serves
    an active and an inactive button.
  * PNGs are emitted with their own colours intact, for artwork that is not a
    single-colour glyph -- the main menu tiles.
"""

import os
import sys

try:
    import cairosvg
    from PIL import Image
except ImportError:
    sys.exit("needs cairosvg and pillow: pip install cairosvg pillow")

import io

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO_ROOT, "assets", "icons")
OUT_C = os.path.join(REPO_ROOT, "src", "gui", "shell", "icons.c")
OUT_H = os.path.join(REPO_ROOT, "src", "gui", "shell", "icons.h")

# (svg file, C identifier, pixel size).
#
# LVGL does not scale these: the size here is the size on the 480x720 panel, so
# the same drawing needed at two sizes appears twice under two names.
#
# The recurring sizes: 26-30 px for a status bar or a row, 34 px for a corner
# button, 46 px inside the control centre's 88 px circles, 56 px for a dialog
# glyph, 96-160 px for a placeholder standing in for missing artwork.
ICONS = [
    ("chevron-left.svg", "chevron_left", 36),
    ("chevron-right.svg", "chevron_right", 36),
    ("check.svg", "check", 36),
    ("close.svg", "close", 36),
    ("folder.svg", "folder", 32),
    ("music-settings.svg", "music_settings", 34),
    ("repeat.svg", "repeat_all", 30),
    ("repeat-1.svg", "repeat_one", 30),
    ("repeat-off.svg", "repeat_off", 30),
    ("play.svg", "play", 40),
    ("pause.svg", "pause", 40),
    ("play-status.svg", "play_status", 26),
    ("pause-status.svg", "pause_status", 26),
    ("database-search.svg", "library_checking", 26),
    ("database-check.svg", "library_checked", 26),
    ("skip-back.svg", "skip_back", 34),
    ("skip-forward.svg", "skip_forward", 34),
    ("music-note.svg", "music_note", 128),
    ("volume.svg", "volume_mute", 30),
    ("volume-1.svg", "volume_low", 30),
    ("volume-2.svg", "volume_high", 30),
    ("home.svg", "home", 26),
    ("power-off.svg", "power_off", 56),
    ("reboot.svg", "reboot", 56),
    ("battery-warning.svg", "battery_warning", 56),
    ("music-2.svg", "music2", 32),
    ("artist.svg", "artist", 32),
    ("artist-album.svg", "artist_album", 32),
    ("genre.svg", "genre", 32),
    ("album.svg", "album", 32),
    ("list-a-z.svg", "sort_az", 34),
    ("list-z-a.svg", "sort_za", 34),
    ("calendar-arrow-up.svg", "sort_date_old", 34),
    ("calendar-arrow-down.svg", "sort_date_new", 34),
    ("year-arrow-down.svg", "sort_year_asc", 34),
    ("year-arrow-up.svg", "sort_year_desc", 34),
    ("album.svg", "album_big", 96),
    ("circle-play.svg", "circle_play", 34),
    ("ellipsis-vertical.svg", "ellipsis_vertical", 30),
    ("list-queue.svg", "list_queue", 34),
    ("list-x.svg", "list_x", 34),
    ("star-x.svg", "star_x", 34),
    ("bluetooth-status.svg", "bluetooth_status", 30),
    ("bluetooth-search.svg", "bluetooth_search", 36),
    ("bluetooth-receiver.svg", "bluetooth_receiver", 36),
    ("change_codec_receiver.svg", "change_codec", 34),
    ("bluetooth-connecting.svg", "bluetooth_connecting", 56),
    ("headphones.svg", "headphones_big", 56),
    ("shift.svg", "shift", 30),
    ("caps-lock.svg", "caps_lock", 30),
    ("delete.svg", "delete", 30),
    ("space.svg", "space", 34),
    ("trash.svg", "trash", 34),
    ("grip-horizontal.svg", "grip", 36),
    ("circle-dashed-check.svg", "select_mode", 34),
    ("sd-card.svg", "sd_card", 128),
    ("shuffle.svg", "shuffle", 30),
    ("shuffle-repeat.svg", "shuffle_repeat", 30),
    ("arrow-down-up.svg", "arrow_down_up", 34),
    ("star.svg", "star", 32),
    ("star-filled.svg", "star_filled", 32),
    ("zap.svg", "zap", 34),
    ("loader-circle.svg", "loader_small", 26),
    ("loader-circle.svg", "loader_big", 56),
    ("airpods.svg", "airpods_hero", (31, 30)),
    ("airpods-gen3.svg", "airpods_gen3_hero", (42, 30)),
    ("airpods-gen4.svg", "airpods_gen4_hero", (40, 30)),
    ("airpods-pro.svg", "airpods_pro_hero", (45, 30)),
    ("airpods-max.svg", "airpods_max_hero", (28, 30)),
    ("airpod-left.svg", "airpod_left", (32, 72)),
    ("airpod-right.svg", "airpod_right", (32, 72)),
    ("airpod-gen3-left.svg", "airpod_gen3_left", (45, 72)),
    ("airpod-gen3-right.svg", "airpod_gen3_right", (45, 72)),
    ("airpods-gen4-left.svg", "airpod_gen4_left", (43, 72)),
    ("airpods-gen4-right.svg", "airpod_gen4_right", (43, 72)),
    ("airpods-pro-left.svg", "airpod_pro_left", (55, 72)),
    ("airpods-pro-right.svg", "airpod_pro_right", (55, 72)),
    ("airpods-chargingcase-fill.svg", "airpods_case", (59, 72)),
    ("airpods-chargingcase-wireless-fill.svg", "airpods_case_wireless", (59, 72)),
    ("airpods-gen3-chargingcase-wireless-fill.svg", "airpods_gen3_case", (85, 72)),
    ("airpods-gen4-chargingcase-wireless-fill.svg", "airpods_gen4_case", (78, 72)),
    ("airpods-pro-chargingcase-wireless-fill.svg", "airpods_pro_case", (94, 72)),
    ("airpods-max.svg", "airpods_max_big", (66, 72)),
    ("zap.svg", "airpods_charge", 20),
    ("noise-cancellation.svg", "airpods_anc", 32),
    ("transparency.svg", "airpods_transparency", 32),
    ("adaptive.svg", "airpods_adaptive", 32),
    ("heart.svg", "heart", 160),
    ("headphones.svg", "headphones", 26),
    ("usb-audio-out.svg", "usbaudioout", 26),
    ("album.svg", "album_corner", 34),
    ("move-vertical.svg", "reorder", 34),
    ("now-playing.svg", "now_playing", 32),
    ("circle-check.svg", "circle_check", 64),
    ("circle-alert.svg", "circle_alert", 64),
]

# Main menu and section tiles, which keep their own colours and are therefore
# listed separately from the recolourable glyphs above. Stored at exactly the
# size they are drawn at: LVGL does not scale them, so a larger bitmap would be
# weight in the binary and nothing else.
MAIN_MENU_ICON_SIZE = 128
SECTION_ICON_SIZE = 112

COLOR_ICONS = [
    ("lossy-quality.svg", "quality_lossy", 26),
    ("cd-quality.svg", "quality_cd", 26),
    ("hifi-quality.svg", "quality_hifi", 26),
    ("dsd-quality.svg", "quality_dsd", 26),
    ("bluetooth-receiver-icon-page.png", "bluetooth_receiver_page", 200),
]


def render(svg_path, size):
    """SVG -> list of BGRA bytes, forced to white so recolouring works.

    `size` is a side in pixels for the square glyphs, or an explicit
    (width, height) for the AirPods artwork, which is not square.
    """
    width, height = size if isinstance(size, tuple) else (size, size)
    png = cairosvg.svg2png(url=svg_path, output_width=width, output_height=height)
    img = Image.open(io.BytesIO(png)).convert("RGBA")

    raw = img.tobytes()  # RGBA, 4 bytes per pixel
    out = bytearray()
    for i in range(0, len(raw), 4):
        # LVGL's ARGB8888 is stored blue, green, red, alpha in memory.
        out += bytes((255, 255, 255, raw[i + 3]))
    return bytes(out), img.width, img.height


def render_color(path, size):
    """PNG or SVG -> list of BGRA bytes with the original colours kept."""
    if path.lower().endswith(".svg"):
        png = cairosvg.svg2png(url=path, output_width=size, output_height=size)
        img = Image.open(io.BytesIO(png)).convert("RGBA")
    else:
        img = Image.open(path).convert("RGBA")
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)

    raw = img.tobytes()  # RGBA, 4 bytes per pixel
    out = bytearray()
    for i in range(0, len(raw), 4):
        # LVGL's ARGB8888 is stored blue, green, red, alpha in memory.
        out += bytes((raw[i + 2], raw[i + 1], raw[i], raw[i + 3]))
    return bytes(out), img.width, img.height


def c_array(data, per_line=12):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + per_line])
        lines.append("\t" + chunk + ",")
    return "\n".join(lines)


def main():
    parts_c = []
    parts_h = []

    entries = [(f, n, s, False) for f, n, s in ICONS]
    entries += [(f, n, size, True) for f, n, size in COLOR_ICONS]

    for filename, name, size, keep_colour in entries:
        path = os.path.join(ICON_DIR, filename)
        if not os.path.exists(path):
            sys.exit("missing icon: %s" % path)

        data, w, h = render_color(path, size) if keep_colour else render(path, size)

        parts_c.append(
            "// %s, %dx%d\n"
            "static const uint8_t icon_%s_data[] = {\n%s\n};\n\n"
            "const lv_image_dsc_t icon_%s = {\n"
            "\t.header = {\n"
            "\t\t.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "\t\t.cf = LV_COLOR_FORMAT_ARGB8888,\n"
            "\t\t.w = %d,\n"
            "\t\t.h = %d,\n"
            "\t\t.stride = %d,\n"
            "\t},\n"
            "\t.data_size = sizeof(icon_%s_data),\n"
            "\t.data = icon_%s_data,\n"
            "};\n"
            % (filename, w, h, name, c_array(data), name, w, h, w * 4, name, name)
        )
        parts_h.append("extern const lv_image_dsc_t icon_%s;" % name)

    header = (
        "/*\n"
        " * GENERATED FILE -- do not edit by hand.\n"
        " * Produced from the SVGs in assets/icons by tools/svg_to_lvgl.py.\n"
        " *\n"
        " * White ARGB8888 bitmaps: tint them with lv_obj_set_style_image_recolor().\n"
        " */\n\n"
    )

    with open(OUT_C, "w") as f:
        f.write(header)
        f.write('#include "icons.h"\n\n#include <stdint.h>\n\n')
        f.write("\n".join(parts_c))

    with open(OUT_H, "w") as f:
        f.write(header)
        f.write("#ifndef ICONS_H\n#define ICONS_H\n\n")
        f.write('#include "lvgl/lvgl.h"\n\n')
        f.write("\n".join(parts_h))
        f.write("\n\n#endif // ICONS_H\n")

    print("wrote %s and %s" % (OUT_C, OUT_H))


if __name__ == "__main__":
    main()
