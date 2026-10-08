#!/usr/bin/env python3
"""True-scale section through one side wall of the cubevox enclosure.

Units are millimetres, y up. The dish floor and outer wall corner sit at the
origin. The inside of the box is +x and the outside is -x.
"""
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.patches import Patch, Polygon, Rectangle

DEFAULT_OUT = Path(__file__).resolve().parents[1] / "renders" / "enclosure-closure-section.png"

DISH_FILL = "#d9d9d9"
LID_FILL = "#9ecae1"
BOOT_FILL = "#fdae6b"
EDGE_LW = 0.8

LID_LIFT = 12.0
BOOT_DROP = -8.0
RIGHT_OFFSET = 46.0
LEFT_OFFSET = 0.0

# Rise per unit depth on each face. 1.0 is 45 degrees, 0.5 is the 2:1 face.
ENCL_YC = 36.0
ENCL_CREST_H = 1.0
ENCL_DEPTH = 0.7
ENCL_K_LOW = math.tan(math.radians(30))
ENCL_K_UP = 1.0

BOOT_H = 2.6
BOOT_GROOVE_DEPTH = 1.4
BOOT_BEAD_PROUD = 1.3
BOOT_LOW_YC = 12.0
BOOT_UP_YC = 44.0
BOOT_K_LOW_FACE = 1.0
BOOT_K_UP_FACE = 0.5
LID_RIM_K_LOW = 0.5
LID_RIM_K_UP = 1.0

CHAMFER_Y = 1.0
CHAMFER_X = 1.0


def profile(x_surface, direction, depth, yc, height, k_low, k_up):
    """Four points for a groove or bead, lower base first.

    direction is +1 when the feature reaches into +x and -1 when into -x.
    height is the opening on the surface.
    """
    y_lo = yc - height / 2
    y_hi = yc + height / 2
    x_tip = x_surface + direction * depth
    return [
        (x_surface, y_lo),
        (x_tip, y_lo + k_low * depth),
        (x_tip, y_hi - k_up * depth),
        (x_surface, y_hi),
    ]


def crest_profile(x_surface, direction, depth, yc, crest_h, k_low, k_up):
    """Four points for a groove or bead, lower base first, sized by its flat crest.

    The flat at the tip is crest_h tall and centred on yc. The faces ramp out
    to the surface.
    """
    y_lo = yc - crest_h / 2
    y_hi = yc + crest_h / 2
    x_tip = x_surface + direction * depth
    return [
        (x_surface, y_lo - k_low * depth),
        (x_tip, y_lo),
        (x_tip, y_hi),
        (x_surface, y_hi + k_up * depth),
    ]


def jag(x_out, x_in, y_from, y_to, step=0.5):
    """Zigzag break edge from (x_out, y_from) to (x_out, y_to)."""
    n = max(1, round(abs(y_to - y_from) / step))
    pts = []
    for i in range(n + 1):
        y = y_from + (y_to - y_from) * i / n
        pts.append((x_out if i % 2 == 0 else x_in, y))
    return pts


def dish_points():
    groove = crest_profile(2.0, -1, ENCL_DEPTH, ENCL_YC, ENCL_CREST_H, ENCL_K_LOW, ENCL_K_UP)
    boot_groove = profile(0.0, +1, BOOT_GROOVE_DEPTH, BOOT_LOW_YC, BOOT_H,
                          BOOT_K_LOW_FACE, BOOT_K_UP_FACE)
    return (
        [(0.0, 0.0)]
        + jag(30.0, 29.5, 0.0, 2.0)
        + [(2.0, 2.0)]
        + groove
        + [(2.0, 40.0 - CHAMFER_Y), (2.0 - CHAMFER_X, 40.0), (0.0, 40.0)]
        + list(reversed(boot_groove))
    )


def lid_points():
    enclosure_bead = crest_profile(2.0, -1, ENCL_DEPTH, ENCL_YC, ENCL_CREST_H, ENCL_K_LOW, ENCL_K_UP)
    rim_groove = profile(0.0, +1, BOOT_GROOVE_DEPTH, BOOT_UP_YC, BOOT_H,
                         LID_RIM_K_LOW, LID_RIM_K_UP)
    return (
        [(0.0, 40.0), (2.0, 40.0)]
        + list(reversed(enclosure_bead))
        + [(2.0, 30.0), (3.2, 30.0), (3.2, 40.0), (30.0, 40.0)]
        + jag(30.0, 29.5, 40.0, 42.0)
        + [(4.0, 42.0), (4.0, 46.0), (0.0, 46.0)]
        + list(reversed(rim_groove))
    )


def boot_points():
    low_bead = profile(0.0, +1, BOOT_BEAD_PROUD, BOOT_LOW_YC, BOOT_H,
                       BOOT_K_LOW_FACE, BOOT_K_UP_FACE)
    up_bead = profile(0.0, +1, BOOT_BEAD_PROUD, BOOT_UP_YC, BOOT_H,
                      LID_RIM_K_LOW, LID_RIM_K_UP)
    return (
        [(-2.0, -2.0)]
        + jag(30.0, 29.5, -2.0, 0.0)
        + [(0.0, 0.0)]
        + low_bead
        + up_bead
        + [(0.0, 46.0), (-2.0, 46.0)]
    )


DISH_PTS = dish_points()
LID_PTS = lid_points()
BOOT_PTS = boot_points()


def draw_parts(ax, ox, lid_dy, boot_dy):
    for pts, fill, dy in (
        (DISH_PTS, DISH_FILL, 0.0),
        (LID_PTS, LID_FILL, lid_dy),
        (BOOT_PTS, BOOT_FILL, boot_dy),
    ):
        shifted = [(x + ox, y + dy) for x, y in pts]
        ax.add_patch(Polygon(shifted, closed=True, facecolor=fill,
                             edgecolor="black", linewidth=EDGE_LW, zorder=3))


def draw_seam(ax, ox):
    ax.plot([ox - 4.0, ox + 30.0], [40.0, 40.0], color="0.3", lw=0.6,
            ls=(0, (3, 2)), zorder=1)


def draw_part_labels(ax, ox, lid_dy):
    ax.annotate("Dish, PETG", xy=(ox + 1.0, 22.0), xytext=(ox + 7.0, 22.0),
                va="center", fontsize=8,
                arrowprops=dict(arrowstyle="-", lw=0.6, color="black"), zorder=5)
    ax.text(ox + 7.0, 47.0 + lid_dy, "Lid, PETG", fontsize=8, va="bottom", zorder=5)
    ax.annotate("Boot,\nTPU-GF", xy=(ox - 1.0, 25.0), xytext=(ox - 5.0, 25.0),
                ha="right", va="center", fontsize=8,
                arrowprops=dict(arrowstyle="-", lw=0.6, color="black"), zorder=5)


def main(out_path):
    fig = plt.figure(figsize=(8.0, 5.6), facecolor="white")
    ax = fig.add_axes([0.01, 0.12, 0.98, 0.80])
    ax.set_xlim(-15.0, 132.0)
    ax.set_ylim(-19.0, 66.0)
    ax.set_aspect("equal", adjustable="box")
    ax.set_axis_off()

    ox_r = RIGHT_OFFSET

    # Exploded (left) and closed (right) panels.
    draw_parts(ax, LEFT_OFFSET, LID_LIFT, BOOT_DROP)
    draw_parts(ax, ox_r, 0.0, 0.0)
    draw_seam(ax, LEFT_OFFSET)
    draw_seam(ax, ox_r)
    draw_part_labels(ax, LEFT_OFFSET, LID_LIFT)
    draw_part_labels(ax, ox_r, 0.0)

    ax.text(-4.5, 40.0, "seam", ha="right", va="center", fontsize=8, zorder=5)
    ax.text(LEFT_OFFSET + 14.0, 63.0, "Exploded (lid +12 mm, boot -8 mm)",
            ha="center", va="bottom", fontsize=10, weight="bold")
    ax.text(ox_r + 14.0, 63.0, "Closed", ha="center", va="bottom",
            fontsize=10, weight="bold")

    # Lid and boot motion arrows in the exploded panel.
    ax.annotate("", xy=(LEFT_OFFSET + 24.0, 55.2), xytext=(LEFT_OFFSET + 24.0, 62.0),
                arrowprops=dict(arrowstyle="-|>", lw=1.0, color="black"), zorder=5)
    ax.text(LEFT_OFFSET + 25.5, 58.5, "lid down 12 mm", fontsize=7.5, va="center")
    ax.annotate("", xy=(LEFT_OFFSET + 24.0, -10.8), xytext=(LEFT_OFFSET + 24.0, -16.5),
                arrowprops=dict(arrowstyle="-|>", lw=1.0, color="black"), zorder=5)
    ax.text(LEFT_OFFSET + 25.5, -13.7, "boot up 8 mm", fontsize=7.5, va="center")

    # Inside and outside arrows under the closed box.
    ax.annotate("", xy=(ox_r - 6.0, -5.0), xytext=(ox_r - 0.5, -5.0),
                arrowprops=dict(arrowstyle="-|>", lw=0.8, color="black"), zorder=5)
    ax.text(ox_r - 6.5, -5.0, "outside", ha="right", va="center", fontsize=7.5)
    ax.annotate("", xy=(ox_r + 10.0, -5.0), xytext=(ox_r + 2.5, -5.0),
                arrowprops=dict(arrowstyle="-|>", lw=0.8, color="black"), zorder=5)
    ax.text(ox_r + 10.5, -5.0, "inside", ha="left", va="center", fontsize=7.5)

    # Scale bar.
    ax.plot([-12.0, -2.0], [-17.0, -17.0], color="black", lw=1.5, zorder=5)
    ax.text(-7.0, -16.2, "10 mm", ha="center", va="bottom", fontsize=7.5)

    # Callouts on the closed panel.
    callouts = [
        (ox_r + 1.6, 36.0, ox_r + 33.0, 35.0,
         "Enclosure-to-enclosure: lid skirt bead in dish groove\n"
         "(straight runs only, h = 0.5-0.9 by coupon)"),
        (ox_r + 1.3, 44.0, ox_r + 33.0, 47.0,
         "Boot upper bead in lid rim groove\n(follows full profile)"),
        (ox_r + 1.0, 12.0, ox_r + 33.0, 14.0,
         "Boot lower bead in dish groove\n(DrumSynthV3 profile)"),
    ]
    for x0, y0, x1, y1, text in callouts:
        ax.plot([x0, x1], [y0, y1], color="black", lw=0.6, zorder=6)
        ax.plot([x0], [y0], marker="o", ms=2, color="black", zorder=6)
        ax.text(x1 + 1.0, y1, text, va="center", fontsize=7.5, zorder=6)

    # Zoomed inset of the y=36 engagement, 3x the main scale.
    rect = Rectangle((ox_r - 0.5, 34.0), 4.0, 4.0, fill=False, ls="--",
                     lw=0.6, ec="0.2", zorder=6)
    ax.add_patch(rect)
    ax_in = ax.inset_axes([ox_r + 12.0, 50.0, 12.0, 12.0], transform=ax.transData)
    draw_parts(ax_in, ox_r, 0.0, 0.0)
    ax_in.set_xlim(ox_r - 0.5, ox_r + 3.5)
    ax_in.set_ylim(34.0, 38.0)
    ax_in.set_aspect("equal")
    ax_in.set_xticks([])
    ax_in.set_yticks([])
    ax_in.text(0.04, 0.92, "3x", transform=ax_in.transAxes, fontsize=8, va="top")

    fig.legend(
        handles=[
            Patch(facecolor=DISH_FILL, edgecolor="black", label="Dish, PETG"),
            Patch(facecolor=LID_FILL, edgecolor="black", label="Lid, PETG"),
            Patch(facecolor=BOOT_FILL, edgecolor="black", label="Boot, TPU-GF"),
        ],
        loc="lower center", ncol=3, frameon=False, fontsize=9,
        bbox_to_anchor=(0.5, 0.0),
    )
    fig.suptitle("cubevox enclosure closure, section through a side wall (mm, true scale)",
                 fontsize=11, y=0.97)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=300, facecolor="white")
    plt.close(fig)
    print(out_path)


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUT)
