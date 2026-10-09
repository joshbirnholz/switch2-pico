#!/usr/bin/env python3
# Usage: make_gc_drawing.py web/gamecube.png Jost[wght].ttf out.js  (Jost: Google Fonts, OFL; needs fonttools, Pillow)
# Builds the GameCube controller drawing for web/index.html: the picture
# (WebP) plus vector lettering and button shapes. The controller's lettering is
# Futura Condensed; Jost (a free Futura revival) Bold, narrowed to Futura
# Condensed proportions (COND), stands in for it.
import base64, io, sys, math
from PIL import Image
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.boundsPen import BoundsPen

SRC, FONT, OUT = sys.argv[1:4]
S = 983 / 473          # same width as the Pro Controller drawing
COND = 0.76            # horizontal scale: Jost -> Futura Condensed proportions
f = lambda v: round(v * S, 1)

font = instantiateVariableFont(TTFont(FONT), {"wght": 700})
gs, cmap, upm = font.getGlyphSet(), font.getBestCmap(), font["head"].unitsPerEm

def text_path(s, cx, cy, size, spacing=0.0):
    """Outline of `s` centered on (cx, cy) (cap height centered), in drawing units."""
    k = size / upm
    names = [cmap[ord(ch)] for ch in s]
    widths = [gs[n].width for n in names]
    total = sum(widths) + spacing * upm * (len(s) - 1)
    cap = font["OS/2"].sCapHeight
    kx = k * COND
    x0 = cx - total * kx / 2
    y0 = cy + cap * k / 2
    pen = SVGPathPen(gs)
    x = 0
    for n, w in zip(names, widths):
        tp = TransformPen(pen, (kx, 0, 0, -k, x0 + x * kx, y0))
        gs[n].draw(tp)
        x += w + spacing * upm
    return pen.getCommands()

# Picture: as given, WebP.
im = Image.open(SRC).convert("RGBA")
buf = io.BytesIO()
im.save(buf, "WEBP", lossless=True)
img = "data:image/webp;base64," + base64.b64encode(buf.getvalue()).decode()
W, H = 983, round(im.height * S)

INK = "#6b7480"   # the drawing's outline grey
def t(s, cx, cy, size, spacing=0.0):
    return f'<path class="lbl" d="{text_path(s, f(cx), f(cy), f(size), spacing)}"/>'

# Positions in the source picture's pixels (473 x 345).
A = (377.3, 117.3, 29)
B = (328.3, 143.3, 17.5)
START = (236.3, 120.3, 13.3)
LS = (95.7, 119, 30)
CS = (312, 222, 20.5)

def circ(b, c):
    return f'<circle class="b" data-b="{b}" cx="{f(c[0])}" cy="{f(c[1])}" r="{f(c[2])}"/>'

def poly(b, pts, cls="b"):
    return f'<path class="{cls}" data-b="{b}" d="M' + " L".join(f"{f(x)},{f(y)}" for x, y in pts) + ' Z"/>'

def rrect(b, x0, y0, x1, y1, r):
    return (f'<rect class="b" data-b="{b}" x="{f(x0)}" y="{f(y0)}" width="{f(x1-x0)}" height="{f(y1-y0)}" '
            f'rx="{f(r)}"/>')

# Kidney buttons: arcs of the ring around A.
def kidney(b, a0, a1, r_in, r_out):
    cx, cy = A[0], A[1]
    pts = []
    n = 12
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        pts.append((cx + r_out * math.cos(a), cy + r_out * math.sin(a)))
    for i in range(n + 1):
        a = math.radians(a1 - (a1 - a0) * i / n)
        pts.append((cx + r_in * math.cos(a), cy + r_in * math.sin(a)))
    return poly(b, pts)

svg = [f'<svg viewBox="0 -56 {W} {H + 56}" aria-hidden="true">']
# Buttons on top of the controller, as pills above the drawing (like ZL / ZR
# on the Pro Controller's): ZL and Z over the shoulders, Capture, Home and C
# between them. Positions in drawing units.
def pill(b, lab, x0, x1):
    svg.append(f'<rect class="pill" x="{x0}" y="-50" width="{x1-x0}" height="38" rx="19"/>'
               f'<text x="{(x0+x1)/2}" y="-31">{lab}</text>'
               f'<rect class="b" data-b="{b}" x="{x0}" y="-50" width="{x1-x0}" height="38" rx="19"/>')
pill("ZL", "ZL", f(55), f(115))
pill("ZR", "Z", f(355), f(430))
for i, (b, lab) in enumerate((("Capture", "Capture"), ("Home", "Home"), ("C", "C"))):
    x0 = 330 + i * 110
    pill(b, lab, x0, x0 + 100)
svg.append(f'<image href="${{GC_IMG}}" x="0" y="0" width="{W}" height="{H}"/>')
# L / R: the grey shoulder buttons.
svg.append(poly("L", [(64, 46), (66, 36), (72, 26), (84, 18), (98, 14), (116, 14), (130, 19), (118, 25), (106, 29), (94, 33), (84, 38), (74, 44)]))
svg.append(poly("R", [(352, 18), (370, 15), (400, 17), (410, 22), (420, 30), (430, 40), (436, 52), (426, 54), (414, 46), (400, 40), (388, 34), (374, 27), (360, 22)]))
svg.append(circ("A", A) + circ("B", B))
# X / Y: traced from the picture (centroid, outline).
YK = ((365.4, 65.5), [(391.4, 65.5), (390.2, 68.8), (387.6, 71.5), (385.7, 74.0), (381.9, 75.0), (378.1, 75.3), (375.3, 75.4), (373.9, 76.6), (371.9, 76.8), (370.0, 76.6), (368.5, 77.1), (367.0, 77.4), (365.4, 77.5), (363.7, 78.4), (361.8, 79.1), (359.3, 80.3), (357.4, 79.4), (353.9, 80.6), (349.2, 81.8), (346.4, 80.2), (343.8, 78.0), (342.3, 75.1), (340.3, 72.3), (339.6, 68.9), (340.4, 65.5), (341.6, 62.4), (344.2, 59.8), (346.9, 57.9), (349.8, 56.5), (351.9, 55.2), (354.1, 54.2), (356.3, 53.6), (358.4, 53.4), (360.4, 53.5), (361.8, 52.0), (363.6, 51.7), (365.4, 51.5), (367.2, 51.7), (369.3, 51.1), (371.5, 50.8), (373.9, 50.8), (376.4, 51.3), (379.6, 51.4), (383.7, 51.5), (387.9, 52.5), (388.5, 56.0), (389.6, 59.1), (391.2, 62.1)])
XK = ((431.4, 107.3), [(445.4, 107.3), (445.3, 109.1), (445.9, 111.2), (446.2, 113.4), (446.1, 115.8), (445.7, 118.2), (445.5, 121.4), (445.4, 125.5), (443.9, 128.9), (441.3, 131.3), (438.1, 132.4), (434.8, 133.0), (431.4, 132.3), (428.1, 132.1), (425.4, 129.5), (423.0, 127.6), (421.9, 123.7), (421.6, 120.0), (420.8, 117.9), (420.3, 115.8), (420.1, 113.8), (420.3, 111.9), (419.8, 110.4), (420.5, 108.7), (419.4, 107.3), (419.5, 105.7), (417.9, 103.6), (418.4, 101.9), (417.5, 99.3), (416.3, 95.7), (415.8, 91.7), (416.8, 88.2), (418.9, 85.6), (421.4, 83.3), (424.6, 82.2), (428.1, 82.5), (431.4, 82.3), (434.5, 83.5), (437.1, 86.0), (439.0, 88.8), (440.9, 90.8), (441.1, 94.6), (442.7, 96.0), (442.5, 98.7), (443.5, 100.3), (444.3, 101.9), (443.9, 103.9), (445.3, 105.4)])
svg.append(poly("Y", YK[1]) + poly("X", XK[1]))
svg.append(circ("Plus", START))
svg.append(rrect("Up", 149, 190, 170, 212, 4) + rrect("Down", 149, 232, 170, 253, 4))
svg.append(rrect("Left", 128.7, 212.3, 150, 232.3, 4) + rrect("Right", 170, 212.3, 192, 232.3, 4))
# Lettering.
svg.append(t("A", A[0], A[1], 28))
svg.append(t("B", B[0], B[1], 18))
svg.append(t("Y", YK[0][0], YK[0][1], 15))
svg.append(t("X", XK[0][0], XK[0][1], 15))
svg.append(t("START/PAUSE", START[0], START[1] - 22, 8.5, 0.03))
# Sticks: the moving caps; the C-stick's carries its "C".
svg.append(f'<circle class="dot" id="dot-l" cx="{f(LS[0])}" cy="{f(LS[1])}" r="{f(LS[2]*0.75)}"/>')
svg.append(f'<g id="dot-r"><circle class="dot" cx="{f(CS[0])}" cy="{f(CS[1])}" r="{f(CS[2]*0.85)}"/>'
           f'<path class="lbl dark" d="{text_path("C", f(CS[0]), f(CS[1]), f(19))}"/></g>')
svg.append("</svg>")

with open(OUT, "w") as o:
    o.write("const GC_IMG='" + img + "';\n")
    o.write("const GC_SVG=`" + "\n".join(svg) + "`;\n")
print(len(img), "bytes of image")
