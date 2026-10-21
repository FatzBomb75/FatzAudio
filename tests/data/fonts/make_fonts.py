#!/usr/bin/env python3
"""Builds the small test fonts for tests/test_font.c with fontTools (pip install fonttools).

Every shape is ours and simple, so the tests can say exactly what each glyph covers:

  tt.ttf      TrueType outlines: a square "A", a circle "O" drawn only with off-curve
              points, a composite "B" (the square, moved and scaled), "V", a glyph
              for U+1F600 (cmap format 12), a kern table (A V = -80).
  tt_gpos.ttf The same with GPOS kerning instead: a pair (A V = -80, format 1)
              and a class pair (O, B against V = -30, format 2) in an extension lookup.
  cff.otf     CFF outlines: the square from a local subroutine, the circle as four
              cubics through a global subroutine, a hintmask, and a flex.
  cid.otf     The CFF font as a CID-keyed font: two font dicts, FDSelect format 3.
  pair.ttc    A collection of tt.ttf and cff.otf.

Run from this folder; the outputs are committed, so the tests need no Python.
"""
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.misc.psCharStrings import T2CharString
from fontTools.ttLib import TTFont
from fontTools.ttLib.ttCollection import TTCollection
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools.cffLib import FDArrayIndex, FontDict, FDSelect, PrivateDict, SubrsIndex
import copy

ORDER = [".notdef", "space", "A", "B", "O", "V", "smile"]
CMAP = {0x20: "space", 0x41: "A", 0x42: "B", 0x4F: "O", 0x56: "V", 0x1F600: "smile"}
ADV = {".notdef": 500, "space": 250, "A": 700, "B": 700, "O": 700, "V": 600, "smile": 800}
NAME = {"familyName": "FA Test", "styleName": "Regular"}


def square(pen, x0, y0, x1, y1):
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()


def common(fb, upem_metrics=True):
    fb.setupHorizontalMetrics({g: (ADV[g], 0) for g in ORDER})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable(NAME)
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200,
                sxHeight=500, sCapHeight=700, usWeightClass=400, version=4)
    fb.setupPost()


def build_tt(gpos):
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(ORDER)
    fb.setupCharacterMap(CMAP)
    glyphs = {}
    for g in ORDER:
        pen = TTGlyphPen(None)
        if g == "A":
            square(pen, 100, 0, 600, 700)                  # 500 x 700
        elif g == "V":
            pen.moveTo((0, 700)); pen.lineTo((300, 0)); pen.lineTo((600, 700)); pen.lineTo((450, 700))
            pen.lineTo((300, 300)); pen.lineTo((150, 700)); pen.closePath()
        elif g == "smile":
            square(pen, 100, 100, 700, 700)
            square(pen, 300, 300, 500, 500)                # a hole: drawn the other way below
        elif g == ".notdef":
            square(pen, 50, 0, 450, 700)
        glyphs[g] = pen.glyph()
    # The hole of "smile" runs the other way round.
    sm = glyphs["smile"]
    coords = list(sm.coordinates)
    end0 = sm.endPtsOfContours[0]
    hole = coords[end0 + 1:]
    hole.reverse()
    sm.coordinates = type(sm.coordinates)(coords[:end0 + 1] + hole)
    # "O": a circle of radius 300 around (350, 350), all eight points off-curve.
    from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates, GlyphComponent
    import math
    o = Glyph()
    o.numberOfContours = 1
    r = 300 / math.cos(math.pi / 8)
    pts = [(round(350 + r * math.cos(math.pi / 8 + k * math.pi / 4)), round(350 + r * math.sin(math.pi / 8 + k * math.pi / 4)))
           for k in range(8)]
    pts.reverse()                                          # clockwise, as TrueType draws outer contours
    o.coordinates = GlyphCoordinates(pts)
    o.flags = bytearray([0] * 8)
    o.endPtsOfContours = [7]
    o.program = None
    from fontTools.ttLib.tables import ttProgram
    o.program = ttProgram.Program()
    o.program.fromBytecode(b"")
    glyphs["O"] = o
    # "B": "A" at half size, moved right 200 and up 100.
    b = Glyph()
    b.numberOfContours = -1
    comp = GlyphComponent()
    comp.glyphName = "A"
    comp.x, comp.y = 200, 100
    comp.flags = 0x0004 | 0x0002                           # ROUND_XY_TO_GRID, ARGS_ARE_XY_VALUES
    comp.transform = [[0.5, 0], [0, 0.5]]
    b.components = [comp]
    glyphs["B"] = b
    fb.setupGlyf(glyphs)
    common(fb)
    font = fb.font
    if gpos:
        addOpenTypeFeaturesFromString(font, """
            languagesystem DFLT dflt;
            lookup pairs useExtension { pos A V -80; } pairs;
            @L = [O B];
            @R = [V];
            lookup classes { pos @L @R -30; } classes;
            feature kern { lookup pairs; lookup classes; } kern;
        """)
    else:
        from fontTools.ttLib import newTable
        kern = newTable("kern")
        kern.version = 0
        st = KernTable_format_0()
        st.version, st.coverage, st.format = 0, 1, 0
        st.kernTable = {("A", "V"): -80, ("V", "A"): -40}
        kern.kernTables = [st]
        font["kern"] = kern
    return font


def cs(program, private=None, gsubrs=None):
    c = T2CharString(program=program, private=private, globalSubrs=gsubrs)
    return c


def build_cff():
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(ORDER)
    fb.setupCharacterMap(CMAP)
    charstrings = {}
    for g in ORDER:
        pen = T2CharStringPen(ADV[g], None)
        if g == "V":
            pen.moveTo((0, 700)); pen.lineTo((300, 0)); pen.lineTo((600, 700)); pen.lineTo((450, 700))
            pen.lineTo((300, 300)); pen.lineTo((150, 700)); pen.closePath()
        elif g == ".notdef":
            square(pen, 50, 0, 450, 700)
        elif g == "smile":
            square(pen, 100, 100, 700, 700)
            pen.moveTo((300, 300)); pen.lineTo((500, 300)); pen.lineTo((500, 500)); pen.lineTo((300, 500)); pen.closePath()
        charstrings[g] = pen.getCharString()
    fb.setupCFF("FATest-Regular", {"FullName": "FA Test"}, charstrings, {})
    common(fb)
    font = fb.font
    cff = font["CFF "].cff
    top = cff.topDictIndex[0]
    priv = top.Private
    gsubrs = cff.GlobalSubrs
    # Local subr 0 (biased -107): the square body, drawn from (100, 0).
    subrs = SubrsIndex()
    subrs.append(T2CharString(program=[0, 700, "rlineto", 500, 0, "rlineto", 0, -700, "rlineto", "return"]))
    priv.Subrs = subrs
    # Global subr 0: four quarter-circle cubics, radius 300 around (350, 350), from (650, 350).
    k = 300 * 0.5523
    gsubrs.append(T2CharString(program=[
        0, k, -(300 - k), 300 - k, -k, 0, "rrcurveto"] + [
        -k, 0, -(300 - k), -(300 - k), 0, -k, "rrcurveto",
        0, -k, 300 - k, -(300 - k), k, 0, "rrcurveto",
        k, 0, 300 - k, 300 - k, 0, k, "rrcurveto", "return"]))
    rounded = [round(v, 2) if isinstance(v, float) else v for v in gsubrs[0].program]
    gsubrs[0].program = rounded
    def charstring(program):
        c = T2CharString(program=program, private=priv, globalSubrs=gsubrs)
        return c
    strings = top.CharStrings
    # "A": width, a stem pair, a hintmask, then the subroutine.
    strings["A"] = charstring([700, 0, 700, "hstemhm", 100, 500, "vstemhm", "hintmask", bytes([0xC0]),
                               100, 0, "rmoveto", -107, "callsubr", "endchar"])
    strings["O"] = charstring([650, 350, "rmoveto", -107, "callgsubr", "endchar"])
    # "B": a flat-topped bump drawn with flex (two curves that are almost a line), then a line back.
    strings["B"] = charstring([100, 0, "rmoveto", 0, 600, "rlineto",
                               100, 20, 100, 20, 100, 0, 100, 0, 100, -20, 100, -20, 50, "flex",
                               0, -600, "rlineto", "endchar"])
    return font


def to_cid(font):
    font = copy.deepcopy(font)
    cff = font["CFF "].cff
    top = cff.topDictIndex[0]
    top.ROS = ("Adobe", "Identity", 0)
    top.CIDCount = len(ORDER)
    fdarray = FDArrayIndex()
    for i in range(2):
        fd = FontDict()
        fd.setCFF2(False)
        priv = copy.deepcopy(top.Private)
        if i == 1:
            # The second dict has no local subrs: "A" moves to the first dict.
            priv.Subrs = SubrsIndex()
            priv.Subrs.append(T2CharString(program=["return"]))
        fd.Private = priv
        fd.FontName = "FATest-%d" % i
        fdarray.append(fd)
    top.FDArray = fdarray
    sel = FDSelect()
    sel.format = 3
    # .notdef space A -> dict 0; B O V smile -> dict 1, except: A needs dict 0's subrs.
    sel.gidArray = [0, 0, 0, 1, 1, 1, 1]
    top.FDSelect = sel
    del top.Private
    index = top.CharStrings.charStringsIndex
    for i in range(len(ORDER)):
        index[i].private = top.FDArray[sel.gidArray[i]].Private
    return font


def main():
    tt = build_tt(False)
    tt.save("tt.ttf")
    build_tt(True).save("tt_gpos.ttf")
    cff = build_cff()
    cff.save("cff.otf")
    to_cid(TTFont("cff.otf")).save("cid.otf")
    col = TTCollection()
    col.fonts = [TTFont("tt.ttf"), TTFont("cff.otf")]
    col.save("pair.ttc")


if __name__ == "__main__":
    main()
