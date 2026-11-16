#!/usr/bin/env python3
"""Draws OpenLamina's icon: a pane of glass, notched at its right-hand corner, held over a blue sheet.

Writes, from the same shapes:
  art/icon/icon.svg            the icon as a vector drawing (256 x 256)
  art/icon/icon.png            512 x 512, for the README
  RuntimeSwitcher/icon.ico     16 to 256 px, the app's icon and the installer's; the small sizes are drawn with a
                               heavier outline and no soft shadows, so the pane still reads at 16 px

Run: python art/icon/make_icon.py   (needs Pillow 9.1 or later)

A shape is (kind, points, fill, stroke colour, stroke width) on a 256 x 256 grid: "poly" (a closed polygon; fill is a
colour, or {"color"} or {"grad": (top, bottom)}, with optional "opacity" and "hide": points to cut out), "path" (an open
line) or "shadow" (fill is {"color", "alpha", "blur", "clip": points}).
"""
import io
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SVG = HERE / "icon.svg"
PNG = HERE / "icon.png"
ICO = ROOT / "RuntimeSwitcher" / "icon.ico"

PNG_SIZE = 512
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]  # small and large icons at 100% to 200% scaling; Explorer scales the 256
SS = 8  # each size is drawn at 8 x 256 px and scaled down


# ---------------------------------------------------------------- geometry

def iso(u, v, cx, top, w, h):
    """A point on a sheet: u runs down-right, v down-left, both 0..1 across the sheet."""
    return (cx + (u - v) * w, top + (u + v) * h)


PLATE_UV = [(0, 0), (1, 0), (1, 1), (0, 1)]


def notched_uv(notch):
    """The glass pane's outline in sheet coordinates: the square without its far right corner."""
    return [(0, 0), (1 - notch, 0), (1 - notch, notch), (1, notch), (1, 1), (0, 1)]


def faces(pts, thick, colours, stroke, sw, notch=frozenset()):
    """A sheet's side faces that face the viewer, as shapes drawn before its top face.

    The edges numbered in `notch` are the cutout's walls. They are one colour, outlined along their lower edge and at
    the cutout's mouth but not where one wall meets the next; and where the sheet's own top face lies over them (nearer
    the viewer), they are hidden, as if on the far side of the glass."""
    shapes = []
    n = len(pts)
    area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n)) / 2
    for i, p1 in enumerate(pts):
        p2 = pts[(i + 1) % n]
        dx, dy = p2[0] - p1[0], p2[1] - p1[1]
        nx, ny = (dy, -dx) if area > 0 else (-dy, dx)
        if ny <= 0:
            continue  # faces away from the viewer
        quad = [p1, p2, (p2[0], p2[1] + thick), (p1[0], p1[1] + thick)]
        if i not in notch:
            shapes.append(("poly", quad, colours["left"] if nx < 0 else colours["right"], stroke, sw))
            continue
        hide = {"hide": pts}
        shapes.append(("poly", quad, {**colours["left"], **hide}, None, 0))
        shapes.append(("path", [quad[3], quad[2]], hide, stroke, sw))
        if (i - 1) % n not in notch:  # this wall starts at the cutout's mouth
            shapes.append(("path", [quad[0], quad[3]], hide, stroke, sw))
        if (i + 1) % n not in notch:  # and this one ends there
            shapes.append(("path", [quad[1], quad[2]], hide, stroke, sw))
    return shapes


def sheet(uv, cx, top, w, h, thick, colours, stroke, sw):
    """A sheet from its outline in sheet coordinates: side faces, then its top face."""
    pts = [iso(u, v, cx, top, w, h) for u, v in uv]
    notch = {i for i, (a, b) in enumerate(zip(uv, uv[1:] + uv[:1]))
             if not any(a[k] == b[k] and a[k] in (0, 1) for k in (0, 1))}  # edges not on the sheet's own square
    return faces(pts, thick, colours, stroke, sw, notch) + [("poly", pts, colours["top"], stroke, sw)]


def icon_shapes(p):
    """The lower sheet, the shadows, the glass pane and its rim, centred in the 256 x 256 grid."""
    cx, w, gap, thick, notch = 128, p["w"], p["gap"], p["thick"], p["notch"]
    h = w / 2
    # Centre the icon vertically, from the pane's top corner to the lower sheet's bottom edge.
    top_up = (256 - (2 * h + thick + gap)) / 2
    low_top = top_up + gap
    pane = notched_uv(notch)

    shapes = sheet(PLATE_UV, cx, low_top, w, h, thick, p["low"], None, 0)
    if p.get("shadow"):  # the pane's shadow, faint and wide, on the lower sheet
        s = p["shadow"]
        lower_face = [iso(u, v, cx, low_top, w, h) for u, v in PLATE_UV]
        footprint = [(x + 3, y + 5) for x, y in (iso(u, v, cx, low_top, w, h) for u, v in pane)]
        shapes.append(("shadow", footprint, dict(s, clip=lower_face), None, 0))
    if p.get("halo"):  # a soft shadow behind the pane, on the background too, so it shows against white
        s = p["halo"]
        face = [iso(u, v, cx, top_up, w, h) for u, v in pane]
        shapes.append(("shadow", [(x, y + 6 + thick / 2) for x, y in face],
                       dict(s, clip=[(0, 0), (256, 0), (256, 256), (0, 256)]), None, 0))
    shapes += sheet(pane, cx, top_up, w, h, thick, p["up"], p["outline"], p["outline_w"])
    if p.get("rim"):  # a thin white highlight along the top face's edge, inside its outline
        face = [iso(u, v, cx, top_up, w, h) for u, v in pane]
        shapes.append(("path", face + face[:2], None, "#FFFFFF", p["rim"]))
    return shapes


# ---------------------------------------------------------------- the icon

def grad(top, bottom):
    return {"grad": (top, bottom)}


def clear(colour, opacity):
    """A see-through flat colour."""
    return {"color": colour, "opacity": opacity}


ICON = dict(
    w=120, gap=48, thick=14, notch=0.5,
    # The lower sheet: sky blue at the back to deep blue at the front.
    low=dict(top=grad("#3AA8FF", "#0E62DB"), left="#0B4FB5", right="#093F94"),
    # The pane: faintly blue glass, clearest at the front, with bluer, more solid sides.
    up=dict(top=grad("#F4F8FFD9", "#D3E4FF73"), left=clear("#A7C6F4", 0.68), right=clear("#8CB2EC", 0.72)),
    outline="#3C86D8", outline_w=3.5, rim=1.4,
    shadow=dict(color="#0B1A5A", alpha=0.12, blur=20),
    halo=dict(color="#0B2A6B", alpha=0.16, blur=9),
)


def params(size):
    """The icon's look at `size` px. Below 128 px the outline is kept about a pixel wide; at 32 px and below the
    shadows and the rim (which only blur there) go, the pane is a little more solid and the sheets a little thicker."""
    p = dict(ICON)
    px = 256 / size  # one pixel, in grid units
    if size < 128:
        p["outline_w"] = max(ICON["outline_w"], 1.0 * px)
        p["rim"] = max(ICON["rim"], 0.45 * px)
    if size <= 32:
        p.update(shadow=None, halo=None, rim=None, outline="#2E72CC", outline_w=1.15 * px, thick=max(14, 1.6 * px),
                 up=dict(ICON["up"], top=grad("#F4F8FFF0", "#D3E4FFB0")))
    return p


# ---------------------------------------------------------------- output

def stop(colour):
    """A gradient stop's colour; #rrggbbaa is split into stop-color and stop-opacity."""
    if len(colour) == 9:
        return f'stop-color="{colour[:7]}" stop-opacity="{int(colour[7:], 16) / 255:.2f}"'
    return f'stop-color="{colour}"'


def fill_attr(fill, gradients):
    """The SVG fill (and fill-opacity) for a colour or gradient; gradients are collected as definitions."""
    if not isinstance(fill, dict):
        return f'fill="{fill}"'
    opacity = f' fill-opacity="{fill["opacity"]:.2f}"' if fill.get("opacity", 1) < 1 else ""
    if "color" in fill:
        return f'fill="{fill["color"]}"{opacity}'
    gid = f"g{len(gradients)}"
    top, bottom = fill["grad"]
    gradients.append(f'<linearGradient id="{gid}" x1="0" y1="0" x2="0" y2="1">'
                     f'<stop offset="0" {stop(top)}/><stop offset="1" {stop(bottom)}/></linearGradient>')
    return f'fill="url(#{gid})"{opacity}'


def points(pts):
    return " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)


def svg(shapes):
    defs, body, gradients = [], [], []
    for kind, pts, fill, stroke, width in shapes:
        if kind == "shadow":
            n = len(defs)
            defs.append(f'<clipPath id="c{n}"><polygon points="{points(fill["clip"])}"/></clipPath>')
            defs.append(f'<filter id="b{n}" x="-30%" y="-30%" width="160%" height="160%">'
                        f'<feGaussianBlur stdDeviation="{fill["blur"]}"/></filter>')
            body.append(f'<g clip-path="url(#c{n})"><polygon points="{points(pts)}" fill="{fill["color"]}" '
                        f'fill-opacity="{fill["alpha"]:.2f}" filter="url(#b{n})"/></g>')
            continue
        line = (f' stroke="{stroke}" stroke-width="{width:.1f}" stroke-linejoin="round" stroke-linecap="round"'
                if stroke and width else "")
        if kind == "path":
            element = f'<polyline points="{points(pts)}" fill="none"{line}/>'
        else:
            element = f'<polygon points="{points(pts)}" {fill_attr(fill, gradients)}{line}/>'
        if isinstance(fill, dict) and fill.get("hide"):  # the area cut out of it, as a mask
            n = len(defs)
            defs.append(f'<mask id="m{n}" maskUnits="userSpaceOnUse" x="0" y="0" width="256" height="256">'
                        f'<rect width="256" height="256" fill="#fff"/><polygon points="{points(fill["hide"])}" '
                        f'fill="#000"/></mask>')
            element = f'<g mask="url(#m{n})">{element}</g>'
        body.append(element)
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">',
             "<title>OpenLamina</title>"]
    parts += ["<defs>"] + gradients + defs + ["</defs>"]
    return "\n".join(parts + body + ["</svg>"]) + "\n"


def render(shapes, size):
    """The shapes drawn at size x size px, on transparent."""
    big = 256 * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))

    def mask_of(pts, value=255):
        m = Image.new("L", img.size, 0)
        ImageDraw.Draw(m).polygon([(x * SS, y * SS) for x, y in pts], fill=value)
        return m

    for kind, pts, fill, stroke, width in shapes:
        big_pts = [(x * SS, y * SS) for x, y in pts]
        if kind == "shadow":
            blurred = mask_of(pts, int(255 * fill["alpha"])).filter(ImageFilter.GaussianBlur(fill["blur"] * SS))
            layer = Image.new("RGBA", img.size, fill["color"])
            layer.putalpha(ImageChops.multiply(blurred, mask_of(fill["clip"])))
            img.alpha_composite(layer)
            continue
        hide = mask_of(fill["hide"]) if isinstance(fill, dict) and fill.get("hide") else None
        if kind == "path":
            px = max(1, round(width * SS))
            m = Image.new("L", img.size, 0)
            d = ImageDraw.Draw(m)
            d.line(big_pts, fill=255, width=px, joint="curve")
            for x, y in (big_pts[0], big_pts[-1]):
                d.ellipse([x - px / 2, y - px / 2, x + px / 2, y + px / 2], fill=255)
            if hide:
                m = ImageChops.subtract(m, hide)
            layer = Image.new("RGBA", img.size, stroke)
            layer.putalpha(m)
            img.alpha_composite(layer)
            continue
        shape = mask_of(pts)
        opacity = fill.get("opacity", 1.0) if isinstance(fill, dict) else 1.0
        if isinstance(fill, dict) and "grad" in fill:
            xs, ys = [q[0] for q in big_pts], [q[1] for q in big_pts]
            x0, y0, x1, y1 = int(min(xs)), int(min(ys)), int(max(xs)) + 1, int(max(ys)) + 1
            ramp = Image.linear_gradient("L").resize((x1 - x0, y1 - y0))
            layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
            layer.paste(Image.composite(Image.new("RGBA", ramp.size, fill["grad"][1]),
                                        Image.new("RGBA", ramp.size, fill["grad"][0]), ramp), (x0, y0))
        else:
            layer = Image.new("RGBA", img.size, fill["color"] if isinstance(fill, dict) else fill)
        if hide:
            shape = ImageChops.subtract(shape, hide)
        layer.putalpha(ImageChops.multiply(layer.getchannel("A"), shape.point(lambda v: int(v * opacity))))
        img.alpha_composite(layer)
        if stroke and width:
            ImageDraw.Draw(img).line(big_pts + big_pts[:2], fill=stroke, width=max(1, round(width * SS)),
                                     joint="curve")
    return img.resize((size, size), Image.Resampling.BOX)


def ico(images):
    """An .ico of the given images: 256 px as PNG, the rest as 32-bit bitmaps (which every Windows API reads)."""
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for img in images:
        w, h = img.size
        if w >= 256:
            buf = io.BytesIO()
            img.save(buf, format="PNG", optimize=True)
            blob = buf.getvalue()
        else:
            pixels = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
            row = (w + 31) // 32 * 4  # the 1-bit AND mask: set where the image is fully transparent
            alpha = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM).getchannel("A").tobytes()
            mask = bytearray(row * h)
            for y in range(h):
                for x in range(w):
                    if alpha[y * w + x] == 0:
                        mask[y * row + x // 8] |= 0x80 >> (x % 8)
            header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
            blob = header + pixels + bytes(mask)
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs)


def main():
    SVG.write_text(svg(icon_shapes(params(256))), encoding="utf-8", newline="\n")
    render(icon_shapes(params(PNG_SIZE)), PNG_SIZE).save(PNG, optimize=True)
    ICO.write_bytes(ico([render(icon_shapes(params(s)), s) for s in ICO_SIZES]))
    print(f"wrote {SVG.relative_to(ROOT)}, {PNG.relative_to(ROOT)} and {ICO.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
