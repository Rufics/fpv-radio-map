#!/usr/bin/env python3
"""Builds the web client's browser icons from one drawing of the FPV Radio Map mark.

    python3 -m pip install pillow
    python3 tools/app-icons/build_icons.py

The mark is a pilot on a terrain ridge under a fan of three signal arcs coloured by the app's
signal scale (strong to threshold), in the ui-kit colours. The script takes no arguments: it
writes only into web/projects/fpv-app/public, whose index.html and manifest.webmanifest name
the same files:

    favicon.svg                    tab icon for browsers that take SVG
    favicon.ico                    16, 32 and 48 px for the rest
    icons/apple-touch-icon.png     180 px, full bleed (iOS rounds the corners itself)
    icons/icon-{192,512}.png       manifest icons, purpose "any"
    icons/icon-maskable-{192,512}.png  manifest icons, purpose "maskable"

The output is committed, so building the client does not need Pillow; run the script again only
after changing the drawing.
"""
import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_DIR = REPO_ROOT / "web" / "projects" / "fpv-app" / "public"

# Design units: every icon is drawn on a 512 x 512 canvas and scaled.
CANVAS = 512.0
BACKGROUND = "#0b0f14"  # --rf-bg
PILOT = "#35d0ba"  # --rf-ok
SIGNAL = ("#f9e46b", "#f28d3c", "#c9417a")  # --rf-heat-1..3
TERRAIN_FILL = "#2a251f"  # --rf-terrain-fill
TERRAIN_LINE = "#b08d63"  # --rf-terrain-line
# The maskable safe zone is a circle of 40 % of the size; the mark is shrunk to fit inside it.
MASKABLE_SCALE = 0.8
SUPERSAMPLE = 4


@dataclass(frozen=True)
class Arc:
    """A signal arc of the fan above the pilot, with round ends."""

    radius: float
    color: str

    # Screen angles (degrees clockwise from 3 o'clock, as Pillow counts them): a 90 degree fan up.
    START = 225.0
    END = 315.0

    def ends(self, x, y, unit=1.0):
        """The start and end points of the arc around the pilot at (x, y), in `unit` scale."""
        radius = self.radius * unit
        return tuple(
            (x + radius * math.cos(math.radians(a)), y + radius * math.sin(math.radians(a)))
            for a in (self.START, self.END)
        )


@dataclass(frozen=True)
class Mark:
    """The geometry of the mark in design units, scaled around the canvas centre."""

    scale: float = 1.0
    corner: float = 112.0

    PILOT_X = 256.0
    PILOT_Y = 352.0
    PILOT_RADIUS = 38.0
    STROKE = 40.0
    RIDGE_LINE = 14.0
    RADII = (108.0, 178.0, 248.0)
    # The ridge the pilot stands on; it runs past the canvas so it still fills the scaled marks.
    RIDGE = ((-256.0, 452.0), (96.0, 404.0), (172.0, 430.0), (256.0, 390.0), (360.0, 436.0), (430.0, 412.0),
             (768.0, 448.0), (768.0, 768.0), (-256.0, 768.0))

    def _scaled(self, value):
        return CANVAS / 2 + (value - CANVAS / 2) * self.scale

    @property
    def pilot(self):
        """Centre and radius of the pilot dot."""
        return self._scaled(self.PILOT_X), self._scaled(self.PILOT_Y), self.PILOT_RADIUS * self.scale

    @property
    def stroke(self):
        return self.STROKE * self.scale

    @property
    def ridge_line(self):
        return self.RIDGE_LINE * self.scale

    @property
    def arcs(self):
        return tuple(Arc(r * self.scale, c) for r, c in zip(self.RADII, SIGNAL, strict=True))

    @property
    def ridge(self):
        return tuple((self._scaled(x), self._scaled(y)) for x, y in self.RIDGE)


class SvgRenderer:
    """Writes the mark as a compact SVG that scales to any tab size."""

    def __init__(self, mark):
        self._mark = mark

    def render(self):
        mark = self._mark
        x, y, r = mark.pilot
        ridge = " ".join(f"{px:g},{py:g}" for px, py in mark.ridge)
        parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {CANVAS:g} {CANVAS:g}">',
            f'<clipPath id="c"><rect width="{CANVAS:g}" height="{CANVAS:g}" rx="{mark.corner:g}"/></clipPath>',
            '<g clip-path="url(#c)">',
            f'<rect width="{CANVAS:g}" height="{CANVAS:g}" fill="{BACKGROUND}"/>',
            (
                f'<polygon points="{ridge}" fill="{TERRAIN_FILL}" stroke="{TERRAIN_LINE}"'
                f' stroke-width="{mark.ridge_line:g}"/>'
            ),
            "</g>",
            f'<g fill="none" stroke-width="{mark.stroke:g}" stroke-linecap="round">',
        ]
        for arc in mark.arcs:
            (x0, y0), (x1, y1) = (self._round(p) for p in arc.ends(x, y))
            path = f"M{x0:g} {y0:g}A{arc.radius:g} {arc.radius:g} 0 0 1 {x1:g} {y1:g}"
            parts.append(f'<path d="{path}" stroke="{arc.color}"/>')
        parts.append("</g>")
        parts.append(f'<circle cx="{x:g}" cy="{y:g}" r="{r:g}" fill="{PILOT}"/>')
        parts.append("</svg>")
        return "\n".join(parts) + "\n"

    @staticmethod
    def _round(point):
        return tuple(round(v, 1) for v in point)


class RasterRenderer:
    """Draws the mark with Pillow, supersampled and scaled down for smooth edges."""

    def __init__(self, mark):
        self._mark = mark

    def render(self, size):
        big = size * SUPERSAMPLE
        unit = big / CANVAS
        image = Image.new("RGBA", (big, big), BACKGROUND)
        draw = ImageDraw.Draw(image)
        self._draw_ridge(draw, unit)
        self._draw_signal(draw, unit)
        shape = Image.new("L", (big, big), 0)
        ImageDraw.Draw(shape).rounded_rectangle((0, 0, big - 1, big - 1), radius=self._mark.corner * unit, fill=255)
        image.putalpha(shape)
        return image.resize((size, size), Image.Resampling.LANCZOS)

    def _draw_ridge(self, draw, unit):
        points = [(px * unit, py * unit) for px, py in self._mark.ridge]
        draw.polygon(points, fill=TERRAIN_FILL)
        draw.line(points[:-2], fill=TERRAIN_LINE, width=round(self._mark.ridge_line * unit), joint="curve")

    def _draw_signal(self, draw, unit):
        x, y, r = (v * unit for v in self._mark.pilot)
        half = self._mark.stroke * unit / 2
        for arc in self._mark.arcs:
            outer = arc.radius * unit + half
            box = (x - outer, y - outer, x + outer, y + outer)
            draw.arc(box, Arc.START, Arc.END, fill=arc.color, width=round(2 * half))
            for ex, ey in arc.ends(x, y, unit):
                self._dot(draw, ex, ey, half, arc.color)
        self._dot(draw, x, y, r, PILOT)

    @staticmethod
    def _dot(draw, x, y, r, color):
        draw.ellipse((x - r, y - r, x + r, y + r), fill=color)


class IconSet:
    """Every icon file the client serves, written into one public folder."""

    ICO_SIZES = (16, 32, 48)

    def __init__(self, target):
        self._target = target
        self._rounded = Mark()
        self._full_bleed = Mark(corner=0.0)
        self._maskable = Mark(scale=MASKABLE_SCALE, corner=0.0)

    def write(self):
        icons = self._target / "icons"
        icons.mkdir(parents=True, exist_ok=True)
        (self._target / "favicon.svg").write_text(SvgRenderer(self._rounded).render(), encoding="utf-8")
        rounded = RasterRenderer(self._rounded)
        rounded.render(256).save(self._target / "favicon.ico", sizes=[(s, s) for s in self.ICO_SIZES])
        self._save(rounded, 192, icons / "icon-192.png")
        self._save(rounded, 512, icons / "icon-512.png")
        self._save(RasterRenderer(self._full_bleed), 180, icons / "apple-touch-icon.png")
        maskable = RasterRenderer(self._maskable)
        self._save(maskable, 192, icons / "icon-maskable-192.png")
        self._save(maskable, 512, icons / "icon-maskable-512.png")

    @staticmethod
    def _save(renderer, size, path):
        renderer.render(size).save(path, optimize=True)


if __name__ == "__main__":
    IconSet(PUBLIC_DIR).write()
