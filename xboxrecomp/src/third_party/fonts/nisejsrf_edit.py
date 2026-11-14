"""NiseJSRF Recomp edit: the V, X and K of NiseJSRF (Yoyo, dafont.com,
"100% Free"), capitals and lowercase, redrawn so they no longer look like U
and H (Miggas' pick of three: "option 2", diagonal arms, the X and K keeping
the font's middle bar; the V a straight left stroke and a diagonal).
usage: python3 nisejsrf_edit.py <NiseJSRF.ttf> <out.ttf>
then make nisejsrf_ttf.h from the output (the bytes as a C array)."""
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
SRC = sys.argv[1]; OUT = sys.argv[2]
CAP, LOW = 875.0, 600.0
# outlines on the capital's 875-high grid; lowercase = the same squeezed to 600
V = {
 2: [(50,0),(50,875),(250,875),(250,270),(720,875),(950,875),(300,0)],                         # stem + diagonal
}
X = {
 2: [(50,875),(250,875),(370,512),(630,512),(750,875),(950,875),(820,437),(950,0),(750,0),(630,362),(370,362),(250,0),(50,0),(180,437)],
}
K = {
 2: [(50,0),(50,875),(250,875),(250,512),(480,512),(730,875),(950,875),(680,437),(950,0),(730,0),(480,362),(250,362),(250,0)],
}
def area(p):
    return sum(p[i][0]*p[(i+1)%len(p)][1]-p[(i+1)%len(p)][0]*p[i][1] for i in range(len(p)))/2
def put(font, gname, pts, h):
    pts = [(x, round(y*h/CAP)) for x, y in pts]
    if area(pts) > 0: pts = pts[::-1]                 # TrueType outer contours run clockwise
    pen = TTGlyphPen(None)
    pen.moveTo(pts[0])
    for p in pts[1:]: pen.lineTo(p)
    pen.closePath()
    g = pen.glyph(); g.recalcBounds(font['glyf'])
    font['glyf'][gname] = g
    adv, lsb = font['hmtx'][gname]
    font['hmtx'][gname] = (adv, g.xMin)
pick = {"V": 2, "X": 2, "K": 2}
f = TTFont(SRC); cm = f.getBestCmap()
for ch, tab in (('V', V), ('X', X), ('K', K)):
    n = pick[ch]
    if n:
        put(f, cm[ord(ch)], tab[n], CAP)
        put(f, cm[ord(ch.lower())], tab[n], LOW)
# the edited font goes by its own name; the copyright line stays
for r in f['name'].names:
    if r.nameID in (1, 4):
        r.string = 'NiseJSRF Recomp edit'
    elif r.nameID in (3, 6):
        r.string = 'NiseJSRFRecompEdit'
    elif r.nameID == 5:
        r.string = 'Version 1.10 (V, X, K redrawn)'
f.save(OUT)
