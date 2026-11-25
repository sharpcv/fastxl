#!/usr/bin/env python3
"""Generates the fastxl test fixtures. Standard library only.

Usage:
    python projects/tests/fixtures/gen_fixtures.py --out projects/tests/fixtures
    python projects/tests/fixtures/gen_fixtures.py --big build/fixtures/big.xlsx --rows 200000 --cols 8
"""

import argparse
import hashlib
import os
import shutil
import sys
import zipfile

FIXED_DATE = (2024, 1, 1, 0, 0, 0)

CONTENT_TYPES_HEAD = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="xml" ContentType="application/xml"/>'
    '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-'
    'officedocument.spreadsheetml.sheet.main+xml"/>'
)
CONTENT_TYPES_TAIL = "</Types>"
WORKSHEET_CONTENT_TYPE = (
    "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"
)
STYLES_CONTENT_TYPE = (
    "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"
)
SST_CONTENT_TYPE = (
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"
)

RELS_NS = "http://schemas.openxmlformats.org/package/2006/relationships"
DOC_REL_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"


class Xlsx:
    def __init__(self):
        self.parts = []

    def add(self, name, text):
        self.parts.append((name, text.encode("utf-8")))

    def save(self, path, compression=zipfile.ZIP_DEFLATED):
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with zipfile.ZipFile(path, "w", compression) as archive:
            for name, data in self.parts:
                info = zipfile.ZipInfo(name, date_time=FIXED_DATE)
                info.compress_type = compression
                archive.writestr(info, data)


def workbook_xml(sheet_names, date1904=False, states=None, with_rels=True, extra_sheets=None,
                 defined_names="", tail="", extra_rels=None):
    props = '<workbookPr date1904="1"/>' if date1904 else ""
    sheets = []
    rels = []
    for index, name in enumerate(sheet_names, start=1):
        state = ""
        if states and states.get(index):
            state = ' state="%s"' % states[index]
        rel = ""
        if with_rels:
            rel = ' r:id="rId%d"' % index
            rels.append(
                '<Relationship Id="rId%d" Type="%s/worksheet" Target="worksheets/sheet%d.xml"/>'
                % (index, DOC_REL_NS, index)
            )
        sheets.append(
            '<sheet name="%s" sheetId="%d"%s%s/>' % (escape_attr(name), index, state, rel)
        )
    for rel_id, rel_type, target in extra_rels or []:
        rels.append(
            '<Relationship Id="%s" Type="%s/%s" Target="%s"/>'
            % (rel_id, DOC_REL_NS, rel_type, target)
        )
    names = '<definedNames>%s</definedNames>' % defined_names if defined_names else ""
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
        'xmlns:r="%s">%s<sheets>%s</sheets>%s%s</workbook>'
        % (DOC_REL_NS, props, "".join(sheets), names, tail)
    ), rels


def escape_attr(text):
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def styles_xml(custom_formats=None):
    custom_formats = custom_formats or {}
    num_fmts = ""
    if custom_formats:
        entries = "".join(
            '<numFmt numFmtId="%d" formatCode="%s"/>' % (fmt_id, escape_attr(code))
            for fmt_id, code in sorted(custom_formats.items())
        )
        num_fmts = '<numFmts count="%d">%s</numFmts>' % (len(custom_formats), entries)
    # cellXfs indices used by the fixtures:
    # 0 General, 1 date(14), 2 datetime(22), 3 time(45), 4 time(46),
    # 5 custom 164, 6 custom 165, 7 bold general.
    xfs = [
        '<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>',
        '<xf numFmtId="14" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>',
        '<xf numFmtId="22" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>',
        '<xf numFmtId="45" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>',
        '<xf numFmtId="46" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>',
        '<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>',
        '<xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>',
        '<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/>',
    ]
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
        + num_fmts
        + '<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font>'
        '<font><b/><sz val="11"/><name val="Calibri"/></font></fonts>'
        '<fills count="2"><fill><patternFill patternType="none"/></fill>'
        '<fill><patternFill patternType="gray125"/></fill></fills>'
        '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
        '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
        '<cellXfs count="%d">%s</cellXfs>'
        '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
        "</styleSheet>" % (len(xfs), "".join(xfs))
    )


def shared_strings_xml(values):
    items = []
    for value in values:
        space = ' xml:space="preserve"' if value != value.strip(" \t\n") else ""
        items.append("<si><t%s>%s</t></si>" % (space, escape_attr(value)))
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="%d" '
        'uniqueCount="%d">%s</sst>' % (len(values), len(values), "".join(items))
    )


def sheet_xml(rows, merge_cells=None, dimension=None):
    body = "".join(rows)
    merges = ""
    if merge_cells:
        merges = '<mergeCells count="%d">%s</mergeCells>' % (
            len(merge_cells),
            "".join('<mergeCell ref="%s"/>' % ref for ref in merge_cells),
        )
    dim = '<dimension ref="%s"/>' % dimension if dimension else ""
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
        + dim
        + '<sheetData>'
        + body
        + "</sheetData>"
        + merges
        + "</worksheet>"
    )


def build_xlsx(sheets, shared_strings=None, styles=True, date1904=False, with_rels=True,
               compression=zipfile.ZIP_DEFLATED):
    """sheets: list of (name, xml)."""
    xlsx = Xlsx()
    names = [name for name, _ in sheets]
    content = [CONTENT_TYPES_HEAD]
    for index in range(1, len(sheets) + 1):
        content.append(
            '<Override PartName="/xl/worksheets/sheet%d.xml" ContentType="%s"/>'
            % (index, WORKSHEET_CONTENT_TYPE)
        )
    if shared_strings is not None:
        content.append(
            '<Override PartName="/xl/sharedStrings.xml" ContentType="%s"/>' % SST_CONTENT_TYPE
        )
    if styles:
        content.append('<Override PartName="/xl/styles.xml" ContentType="%s"/>' % STYLES_CONTENT_TYPE)
    content.append(CONTENT_TYPES_TAIL)
    xlsx.add("[Content_Types].xml", "".join(content))

    xlsx.add(
        "_rels/.rels",
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="%s"><Relationship Id="rId1" Type="%s/officeDocument" '
        'Target="xl/workbook.xml"/></Relationships>' % (RELS_NS, DOC_REL_NS),
    )

    workbook, rels = workbook_xml(names, date1904=date1904, with_rels=with_rels)
    xlsx.add("xl/workbook.xml", workbook)
    if with_rels:
        extra = []
        if shared_strings is not None:
            extra.append(
                '<Relationship Id="rIdSst" Type="%s/sharedStrings" Target="sharedStrings.xml"/>'
                % DOC_REL_NS
            )
        if styles:
            extra.append(
                '<Relationship Id="rIdStyles" Type="%s/styles" Target="styles.xml"/>' % DOC_REL_NS
            )
        xlsx.add(
            "xl/_rels/workbook.xml.rels",
            '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="%s">%s%s</Relationships>'
            % (RELS_NS, "".join(rels), "".join(extra)),
        )
    if shared_strings is not None:
        xlsx.add("xl/sharedStrings.xml", shared_strings_xml(shared_strings))
    if styles:
        xlsx.add("xl/styles.xml", styles_xml({164: "yyyy-mm-dd", 165: "yyyy-mm-dd hh:mm:ss"}))
    for index, (_, xml) in enumerate(sheets, start=1):
        xlsx.add("xl/worksheets/sheet%d.xml" % index, xml)
    return xlsx


def basic_fixtures():
    # Column layout: A=id, B=name, C=amount, D=active, E=note, F=total.
    shared = ["张三", "Bob", "Alice"]
    header_cells = [
        ("A1", "id"),
        ("B1", "name"),
        ("C1", "amount"),
        ("D1", "active"),
        ("E1", "note"),
        ("F1", "total"),
    ]
    header = "<row r=\"1\">" + "".join(
        '<c r="%s" t="inlineStr"><is><t>%s</t></is></c>' % (ref, escape_attr(text))
        for ref, text in header_cells
    ) + "</row>"
    row2 = (
        '<row r="2"><c r="A2"><v>1</v></c><c r="B2" t="s"><v>0</v></c>'
        '<c r="C2"><v>10.5</v></c><c r="D2" t="b"><v>1</v></c>'
        '<c r="E2" t="inlineStr"><is><t xml:space="preserve"> x </t></is></c>'
        '<c r="F2"><f>SUM(C2:C3)</f><v>8.5</v></c></row>'
    )
    # Row 3 omits the row/cell references for some cells.
    row3 = (
        '<row><c><v>2</v></c><c t="s"><v>1</v></c><c><v>-2</v></c>'
        '<c t="b"><v>0</v></c><c t="e"><v>#DIV/0!</v></c><c/></row>'
    )
    row4 = (
        '<row r="4"><c r="A4"><v>3</v></c><c r="B4" t="s"><v>2</v></c>'
        '<c r="C4"><v>1e3</v></c><c r="D4" t="b"><v>1</v></c>'
        '<c r="E4" t="s"><v>3</v></c></row>'
    )
    # Row 5 is entirely absent.
    row6 = '<row r="6"><c r="C6"><v>7</v></c></row>'
    sheet1 = sheet_xml([header, row2, row3, row4, row6])
    sheet2 = sheet_xml(
        ['<row r="1"><c r="A1" t="inlineStr"><is><t>\U0001F600 emoji</t></is></c>'
         '<c r="B1" t="inlineStr"><is><t>\u0645\u0631\u062d\u0628\u0627</t></is></c></row>']
    )
    shared_all = shared + ["\u00a0"]
    return build_xlsx(
        [("Data", sheet1), ("Ünïcode-中文", sheet2)], shared_strings=shared_all
    )


def dates_fixture(date1904=False):
    rows = []
    serials = [1, 59, 60, 61, 44927, 44927.5] if not date1904 else [0, 1, 100]
    for index, serial in enumerate(serials, start=1):
        rows.append(
            '<row r="%d"><c r="A%d" s="1"><v>%s</v></c></row>'
            % (index, index, format_serial(serial))
        )
    if date1904:
        return build_xlsx([("Dates", sheet_xml(rows))], date1904=True)
    rows.append(
        '<row r="7"><c r="A7" s="2"><v>44927.5</v></c>'
        '<c r="B7" s="3"><v>0.5</v></c><c r="C7" s="4"><v>0.25</v></c>'
        '<c r="D7" s="5"><v>44927</v></c><c r="E7" s="6"><v>44927.5</v></c>'
        '<c r="F7" t="d"><v>2024-01-02T03:04:05</v></c>'
        '<c r="G7" t="d"><v>2024-01-02</v></c></row>'
    )
    return build_xlsx([("Dates", sheet_xml(rows))], date1904=date1904)


def format_serial(value):
    if float(value) == int(value):
        return str(int(value))
    return repr(float(value))


def rich_sst_fixture():
    sst = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="4" '
        'uniqueCount="4">'
        '<si><r><t>Hello</t></r><r><rPr><b/></rPr><t xml:space="preserve"> world</t></r></si>'
        "<si><t>_x000D_CR</t></si>"
        "<si><r><t>\u4e2d</t></r><rPh sb=\"0\" eb=\"1\"><t>zhong</t></rPh><r><t>\u6587</t></r></si>"
        "<si/>"
        "</sst>"
    )
    sheet = sheet_xml(
        ['<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c>'
         '<c r="C1" t="s"><v>2</v></c><c r="D1" t="s"><v>3</v></c></row>']
    )
    xlsx = build_xlsx([("Rich", sheet)])
    xlsx.parts = [(name, data) for name, data in xlsx.parts if name != "xl/sharedStrings.xml"]
    xlsx.add("xl/sharedStrings.xml", sst)
    return xlsx


def ragged_fixture():
    rows = [
        '<row r="1"><c r="A1"><v>1</v></c><c r="B1"><v>2</v></c><c r="C1"><v>3</v></c></row>',
        '<row r="3"><c r="B3"><v>5</v></c><c r="D3"><v>6</v></c></row>',
        '<row r="4"/>',
        '<row><c><v>7</v></c></row>',
    ]
    return build_xlsx([("Ragged", sheet_xml(rows, merge_cells=["A1:B1", "C3:D3"]))])


def no_rels_fixture():
    rows = [
        '<row r="1"><c r="A1"><v>1</v></c><c r="B1"><v>2</v></c></row>',
    ]
    return build_xlsx([("First", sheet_xml(rows)), ("Second", sheet_xml(rows))],
                      with_rels=False, styles=False)



# ---------------------------------------------------------------------------
# Feature fixtures for the verification pass (metadata, multi-sheet, large,
# search, regions, budget). Built from the same minimal OOXML parts as the
# older fixtures so the generator stays standard-library only.

PACKAGE_CONTENT_TYPES = {
    "table": "application/vnd.openxmlformats-officedocument.spreadsheetml.table+xml",
    "comments": "application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml",
    "pivotTable": "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotTable+xml",
    "pivotCacheDefinition": (
        "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml"
    ),
    "drawing": "application/vnd.openxmlformats-officedocument.drawing+xml",
    "chart": "application/vnd.openxmlformats-officedocument.drawingml.chart+xml",
    "theme": "application/vnd.openxmlformats-officedocument.theme+xml",
}

MAIN_NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"


def cell_text(ref, text, style=None):
    s = ' s="%d"' % style if style is not None else ""
    return '<c r="%s"%s t="inlineStr"><is><t xml:space="preserve">%s</t></is></c>' % (
        ref,
        s,
        escape_attr(text),
    )


def cell_number(ref, value, style=None):
    s = ' s="%d"' % style if style is not None else ""
    return '<c r="%s"%s><v>%s</v></c>' % (ref, s, format_serial(value))


def cell_bool(ref, value):
    return '<c r="%s" t="b"><v>%d</v></c>' % (ref, 1 if value else 0)


def cell_formula(ref, formula, cached, style=None):
    s = ' s="%d"' % style if style is not None else ""
    return '<c r="%s"%s><f>%s</f><v>%s</v></c>' % (ref, s, escape_attr(formula), cached)


def row_xml(number, cells, hidden=False):
    attr = ' hidden="1"' if hidden else ""
    return '<row r="%d"%s>%s</row>' % (number, attr, "".join(cells))


def build_package(sheets, extras=None, shared_strings=None, date1904=False, states=None,
                  defined_names="", workbook_tail="", workbook_rels=None, styles=None):
    """sheets: list of (name, sheet_xml, sheet_rels_xml or None).

    extras: (part_path, content_type_key or None, xml).
    """
    xlsx = Xlsx()
    names = [entry[0] for entry in sheets]
    content = [CONTENT_TYPES_HEAD]
    for index in range(1, len(sheets) + 1):
        content.append(
            '<Override PartName="/xl/worksheets/sheet%d.xml" ContentType="%s"/>'
            % (index, WORKSHEET_CONTENT_TYPE)
        )
    if shared_strings is not None:
        content.append(
            '<Override PartName="/xl/sharedStrings.xml" ContentType="%s"/>' % SST_CONTENT_TYPE
        )
    content.append(
        '<Override PartName="/xl/styles.xml" ContentType="%s"/>' % STYLES_CONTENT_TYPE
    )
    for part, kind, _ in extras or []:
        if kind is None:
            continue
        content.append(
            '<Override PartName="/%s" ContentType="%s"/>' % (part, PACKAGE_CONTENT_TYPES[kind])
        )
    content.append(CONTENT_TYPES_TAIL)
    xlsx.add("[Content_Types].xml", "".join(content))
    xlsx.add(
        "_rels/.rels",
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="%s"><Relationship Id="rId1" Type="%s/officeDocument" '
        'Target="xl/workbook.xml"/></Relationships>' % (RELS_NS, DOC_REL_NS),
    )
    workbook, rels = workbook_xml(
        names,
        date1904=date1904,
        states=states,
        defined_names=defined_names,
        tail=workbook_tail,
        extra_rels=workbook_rels,
    )
    xlsx.add("xl/workbook.xml", workbook)
    extra = []
    if shared_strings is not None:
        extra.append(
            '<Relationship Id="rIdSst" Type="%s/sharedStrings" Target="sharedStrings.xml"/>'
            % DOC_REL_NS
        )
    extra.append(
        '<Relationship Id="rIdStyles" Type="%s/styles" Target="styles.xml"/>' % DOC_REL_NS
    )
    xlsx.add(
        "xl/_rels/workbook.xml.rels",
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="%s">%s%s</Relationships>'
        % (RELS_NS, "".join(rels), "".join(extra)),
    )
    if shared_strings is not None:
        xlsx.add("xl/sharedStrings.xml", shared_strings_xml(shared_strings))
    xlsx.add(
        "xl/styles.xml",
        styles
        if styles is not None
        else styles_xml({164: "yyyy-mm-dd", 165: "yyyy-mm-dd hh:mm:ss"}),
    )
    for index, (_, xml, sheet_rels) in enumerate(sheets, start=1):
        xlsx.add("xl/worksheets/sheet%d.xml" % index, xml)
        if sheet_rels:
            xlsx.add("xl/worksheets/_rels/sheet%d.xml.rels" % index, sheet_rels)
    for part, _, xml in extras or []:
        xlsx.add(part, xml)
    return xlsx


def rels_xml(entries, base=RELS_NS):
    """entries: (id, type suffix, target)."""
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="%s">%s</Relationships>'
        % (
            base,
            "".join(
                '<Relationship Id="%s" Type="%s/%s" Target="%s"/>'
                % (rel_id, DOC_REL_NS, suffix, target)
                for rel_id, suffix, target in entries
            ),
        )
    )



def theme_xml():
    """Office theme colour scheme: theme indices 0..11 as Excel orders them."""
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
        'name="Office"><a:themeElements><a:clrScheme name="Office">'
        '<a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1>'
        '<a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1>'
        '<a:dk2><a:srgbClr val="44546A"/></a:dk2>'
        '<a:lt2><a:srgbClr val="E7E6E6"/></a:lt2>'
        '<a:accent1><a:srgbClr val="4472C4"/></a:accent1>'
        '<a:accent2><a:srgbClr val="ED7D31"/></a:accent2>'
        '<a:accent3><a:srgbClr val="A5A5A5"/></a:accent3>'
        '<a:accent4><a:srgbClr val="FFC000"/></a:accent4>'
        '<a:accent5><a:srgbClr val="5B9BD5"/></a:accent5>'
        '<a:accent6><a:srgbClr val="70AD47"/></a:accent6>'
        '<a:hlink><a:srgbClr val="0563C1"/></a:hlink>'
        '<a:folHlink><a:srgbClr val="954F72"/></a:folHlink>'
        "</a:clrScheme></a:themeElements></a:theme>"
    )


def metadata_styles():
    # cellXfs: 0 general, 1 percent, 2 red font, 3 yellow fill, 4 strike.
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<styleSheet xmlns="%s">'
        '<numFmts count="1"><numFmt numFmtId="164" formatCode="0.00%%"/></numFmts>'
        '<fonts count="6">'
        '<font><sz val="11"/><name val="Calibri"/></font>'
        '<font><b/><sz val="11"/><name val="Calibri"/></font>'
        '<font><sz val="11"/><color rgb="FFFF0000"/><name val="Calibri"/></font>'
        '<font><strike/><sz val="11"/><name val="Calibri"/></font>'
        '<font><sz val="11"/><color theme="4"/><name val="Calibri"/></font>'
        '<font><sz val="11"/><color theme="4" tint="-0.25"/><name val="Calibri"/></font>'
        "</fonts>"
        '<fills count="4">'
        '<fill><patternFill patternType="none"/></fill>'
        '<fill><patternFill patternType="gray125"/></fill>'
        '<fill><patternFill patternType="solid">'
        '<fgColor rgb="FFFFFF00"/><bgColor indexed="64"/></patternFill></fill>'
        '<fill><patternFill patternType="solid">'
        '<fgColor theme="4" tint="0.4"/><bgColor indexed="64"/></patternFill></fill>'
        "</fills>"
        '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
        '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
        '<cellXfs count="8">'
        '<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
        '<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
        '<xf numFmtId="0" fontId="2" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="0" fillId="2" borderId="0" xfId="0" applyFill="1"/>'
        '<xf numFmtId="0" fontId="3" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="4" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="5" fillId="0" borderId="0" xfId="0" applyFont="1"/>'
        '<xf numFmtId="0" fontId="0" fillId="3" borderId="0" xfId="0" applyFill="1"/>'
        "</cellXfs>"
        '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
        "</styleSheet>" % MAIN_NS
    )


def metadata_fixture():
    """Styles, names, merges, comments, table, pivot, chart, filter, validation."""
    rows = [
        row_xml(1, [cell_text("A1", "id"), cell_text("B1", "name"),
                    cell_text("C1", "amount"), cell_text("D1", "status"),
                    cell_text("E1", "note")]),
        row_xml(2, [cell_number("A2", 1), cell_text("B2", "Alice"), cell_number("C2", 10.5),
                    cell_text("D2", "paid"), cell_text("E2", "first", style=4)]),
        row_xml(3, [cell_number("A3", 2), cell_text("B3", "Bob", style=2), cell_number("C3", -2),
                    cell_text("D3", "open")]),
        row_xml(4, [cell_number("A4", 3), cell_text("B4", "Carol", style=3), cell_number("C4", 0),
                    cell_text("D4", "paid")], hidden=True),
        row_xml(5, [cell_text("A5", "Totals")]),
        row_xml(6, [cell_formula("C6", "SUM(C2:C4)", "8.5"),
                    cell_number("E6", 0.25, style=1)]),
    ]
    sheet = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<worksheet xmlns="%s" xmlns:r="%s">'
        '<dimension ref="A1:E6"/>'
        '<sheetViews><sheetView workbookViewId="0">'
        '<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>'
        "</sheetView></sheetViews>"
        '<cols><col min="1" max="1" width="6"/><col min="5" max="5" hidden="1" width="24"/></cols>'
        "<sheetData>%s</sheetData>"
        '<autoFilter ref="A1:E4"><filterColumn colId="3">'
        '<filters><filter val="paid"/></filters></filterColumn></autoFilter>'
        '<mergeCells count="2"><mergeCell ref="A5:C5"/><mergeCell ref="D5:E5"/></mergeCells>'
        '<dataValidations count="1"><dataValidation type="list" allowBlank="1" sqref="D2:D4">'
        '<formula1>"paid,open,pending"</formula1></dataValidation></dataValidations>'
        '<tableParts count="1"><tablePart r:id="rId1"/></tableParts>'
        "</worksheet>" % (MAIN_NS, DOC_REL_NS, "".join(rows))
    )
    sheet_rels = rels_xml([
        ("rId1", "table", "../tables/table1.xml"),
        ("rId2", "comments", "../comments1.xml"),
        ("rId3", "pivotTable", "../pivotTables/pivotTable1.xml"),
        ("rId4", "drawing", "../drawings/drawing1.xml"),
    ])
    table = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<table xmlns="%s" id="1" name="Sales" displayName="Sales" ref="A1:C4" '
        'headerRowCount="1"><tableColumns count="3">'
        '<tableColumn id="1" name="id"/><tableColumn id="2" name="name"/>'
        '<tableColumn id="3" name="amount"/></tableColumns></table>' % MAIN_NS
    )
    comments = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<comments xmlns="%s"><authors><author>Alice</author><author>Bob</author></authors>'
        "<commentList>"
        '<comment ref="B2" authorId="0"><text><r><t>checked</t></r></text></comment>'
        '<comment ref="C3" authorId="1"><text><r><t>recheck</t></r></text></comment>'
        "</commentList></comments>" % MAIN_NS
    )
    pivot = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<pivotTableDefinition xmlns="%s" name="PivotTable1" cacheId="1" dataCaption="Values"/>'
        % MAIN_NS
    )
    cache = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<pivotCacheDefinition xmlns="%s"><cacheSource type="worksheet">'
        '<worksheetSource sheet="Data" ref="A1:D4"/></cacheSource></pivotCacheDefinition>'
        % MAIN_NS
    )
    drawing = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<xdr:wsDr xmlns:xdr="http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing" '
        'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
        'xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" '
        'xmlns:r="%s">'
        "<xdr:twoCellAnchor><xdr:graphicFrame><xdr:nvGraphicFramePr/><xdr:xfrm/>"
        '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">'
        '<c:chart r:id="rId1"/></a:graphicData></a:graphic>'
        "</xdr:graphicFrame><xdr:clientData/></xdr:twoCellAnchor></xdr:wsDr>" % DOC_REL_NS
    )
    drawing_rels = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="%s"><Relationship Id="rId1" Type="%s/chart" '
        'Target="../charts/chart1.xml"/></Relationships>' % (RELS_NS, DOC_REL_NS)
    )
    chart = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" '
        'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
        "<c:chart><c:plotArea><c:barChart><c:ser><c:idx val=\"0\"/>"
        "<c:val><c:numRef><c:f>Data!$C$2:$C$4</c:f></c:numRef></c:val>"
        "</c:ser></c:barChart></c:plotArea></c:chart></c:chartSpace>"
    )
    themed_rows = [
        row_xml(1, [cell_text("A1", "label"), cell_text("B1", "amount")]),
        row_xml(2, [cell_text("A2", "accent", style=5), cell_number("B2", 1)]),
        row_xml(3, [cell_text("A3", "tinted"), cell_number("B3", 2, style=6)]),
        row_xml(4, [cell_text("A4", "filled"), cell_number("B4", 3, style=7)]),
    ]
    themed_sheet = sheet_xml(themed_rows)
    extras = [
        ("xl/theme/theme1.xml", "theme", theme_xml()),
        ("xl/tables/table1.xml", "table", table),
        ("xl/comments1.xml", "comments", comments),
        ("xl/pivotTables/pivotTable1.xml", "pivotTable", pivot),
        ("xl/pivotCache/pivotCacheDefinition1.xml", "pivotCacheDefinition", cache),
        ("xl/drawings/drawing1.xml", "drawing", drawing),
        ("xl/drawings/_rels/drawing1.xml.rels", None, drawing_rels),
        ("xl/charts/chart1.xml", "chart", chart),
    ]
    defined_names = (
        '<definedName name="Rate">Data!$C$2</definedName>'
        '<definedName name="Local" localSheetId="0">Data!$D$2</definedName>'
    )
    tail = '<pivotCaches><pivotCache cacheId="1" r:id="rIdPivotCache"/></pivotCaches>'
    workbook_rels = [
        ("rIdPivotCache", "pivotCacheDefinition", "pivotCache/pivotCacheDefinition1.xml"),
        ("rIdTheme", "theme", "theme/theme1.xml"),
    ]
    return build_package(
        [("Data", sheet, sheet_rels), ("Themed", themed_sheet, None)],
        extras=extras,
        defined_names=defined_names,
        workbook_tail=tail,
        workbook_rels=workbook_rels,
        styles=metadata_styles(),
    )


def multi_fixture():
    orders = sheet_xml([
        row_xml(1, [cell_text("A1", "order_id"), cell_text("B1", "customer"),
                    cell_text("C1", "total")]),
        row_xml(2, [cell_number("A2", 1), cell_text("B2", "Alice"), cell_number("C2", 10)]),
        row_xml(3, [cell_number("A3", 2), cell_text("B3", "Bob"), cell_number("C3", 20)]),
        row_xml(4, [cell_number("A4", 3), cell_text("B4", "Carol"), cell_number("C4", 30)]),
    ])
    items = sheet_xml([
        row_xml(1, [cell_text("A1", "order_id"), cell_text("B1", "sku"), cell_text("C1", "qty")]),
        row_xml(2, [cell_number("A2", 1), cell_text("B2", "A-1"), cell_number("C2", 1)]),
        row_xml(3, [cell_number("A3", 1), cell_text("B3", "A-2"), cell_number("C3", 2)]),
        row_xml(4, [cell_number("A4", 2), cell_text("B4", "B-1"), cell_number("C4", 5)]),
    ])
    scratch = sheet_xml([row_xml(1, [cell_text("A1", "secret"), cell_number("B1", 7)])])
    archived = sheet_xml([row_xml(1, [cell_text("A1", "archived")])])
    return build_package(
        [("Orders", orders, None), ("Items", items, None), ("Scratch", scratch, None),
         ("Archived", archived, None)],
        states={3: "hidden", 4: "veryHidden"},
    )


def large_fixture(rows=15000, cols=10):
    """A sheet big enough to exercise streamed profiling and row reads."""
    lines = []
    for row in range(1, rows + 1):
        cells = [cell_number("A%d" % row, row)]
        cells.append(cell_text("B%d" % row, "name-%d" % (row % 997)))
        cells.append(cell_number("C%d" % row, (row % 1000) * 0.5))
        cells.append(cell_bool("D%d" % row, row % 3 == 0))
        cells.append(cell_text("E%d" % row, "bucket-%d" % (row % 12)))
        for col in range(6, cols + 1):
            cells.append(cell_number("%s%d" % (column_name(col - 1), row), row * col))
        lines.append(row_xml(row, cells))
    return build_package([("Data", sheet_xml(lines), None)])


def formulas_fixture():
    """Shared formulas: one master cell, followers only carry si/cached value."""
    rows = [
        row_xml(1, [cell_text("A1", "input"), cell_text("B1", "double"),
                    cell_text("C1", "running"), cell_text("D1", "mixed")]),
        row_xml(2, [
            cell_number("A2", 1),
            '<c r="B2"><f t="shared" ref="B2:B4" si="0">A2*2</f><v>2</v></c>',
            '<c r="C2"><f t="shared" ref="C2:C4" si="1">SUM($A2:B2)</f><v>3</v></c>',
            '<c r="D2"><f t="shared" ref="D2:D4" si="2">$A2+A$2</f><v>2</v></c>',
        ]),
        row_xml(3, [
            cell_number("A3", 2),
            '<c r="B3"><f t="shared" si="0"/><v>4</v></c>',
            '<c r="C3"><f t="shared" si="1"/><v>9</v></c>',
            '<c r="D3"><f t="shared" si="2"/><v>3</v></c>',
        ]),
        row_xml(4, [
            cell_number("A4", 3),
            '<c r="B4"><f t="shared" si="0"/><v>6</v></c>',
            '<c r="C4"><f t="shared" si="1"/><v>18</v></c>',
            '<c r="D4"><f t="shared" si="2"/><v>4</v></c>',
        ]),
    ]
    return build_package([("Formula", sheet_xml(rows), None)])


def search_fixture():
    invoices = sheet_xml([
        row_xml(1, [cell_text("A1", "invoice_id"), cell_text("B1", "customer"),
                    cell_text("C1", "amount"), cell_text("D1", "note")]),
        row_xml(2, [cell_number("A2", 9001), cell_text("B2", "Alice"),
                    cell_number("C2", 120.5), cell_text("D2", "invoice paid")]),
        row_xml(3, [cell_number("A3", 9002), cell_text("B3", "Bob"), cell_number("C3", 42),
                    cell_text("D3", "INVOICED twice")]),
        row_xml(4, [cell_number("A4", 9003), cell_text("B4", "\u4ed8\u6b3e\u53d1\u7968"),
                    cell_number("C4", 4200), cell_text("D4", "\u53d1\u7968\u5df2\u5f00")]),
    ])
    log = sheet_xml([
        row_xml(1, [cell_text("A1", "memo"), cell_text("B1", "cached")]),
        row_xml(2, [cell_text("A2", "row 2"),
                    cell_formula("B2", '"invoice archive"', "hidden")]),
    ])
    return build_package([("Invoices", invoices, None), ("Log", log, None)])


def regions_fixture():
    block1 = [
        row_xml(1, [cell_text("A1", "id"), cell_text("B1", "name"),
                    cell_text("C1", "amount"), cell_text("D1", "status")]),
        row_xml(2, [cell_number("A2", 1), cell_text("B2", "Alice"), cell_number("C2", 10),
                    cell_text("D2", "paid")]),
        row_xml(3, [cell_number("A3", 2), cell_text("B3", "Bob"), cell_number("C3", 20),
                    cell_text("D3", "open")]),
    ]
    block2 = [
        row_xml(6, [cell_text("A6", "sku"), cell_text("B6", "qty"), cell_text("C6", "price")]),
        row_xml(7, [cell_text("A7", "A-1"), cell_number("B7", 3), cell_number("C7", 1.5)]),
        row_xml(8, [cell_text("A8", "A-2"), cell_number("B8", 4), cell_number("C8", 2.5)]),
    ]
    return build_package([("Blocks", sheet_xml(block1 + block2), None)])


def budget_fixture(width=40, rows=6):
    header = [cell_text("%s1" % letter, "col%d" % index)
              for index, letter in enumerate((column_name(i) for i in range(width)), start=1)]
    body = []
    for row in range(2, rows + 2):
        cells = [cell_number("A%d" % row, row - 1),
                 cell_text("B%d" % row, "value-%d" % row),
                 cell_number("C%d" % row, (row - 1) * 2.5)]
        for col in range(4, 10):
            cells.append(cell_text("%s%d" % (column_name(col - 1), row),
                                   "sample-%d-%d" % (row, col)))
        # Columns 10..30 stay empty, column 31 is constant, 32.. are text.
        cells.append(cell_text("AE%d" % row, "fixed"))
        for col in range(33, width + 1):
            cells.append(cell_number("%s%d" % (column_name(col - 1), row), col * 100 + row))
        body.append(row_xml(row, cells))
    return build_package([("Wide", sheet_xml([row_xml(1, header)] + body), None)])


def make_fixtures(out_dir):
    os.makedirs(out_dir, exist_ok=True)

    basic_fixtures().save(os.path.join(out_dir, "basic.xlsx"))
    shutil.copyfile(
        os.path.join(out_dir, "basic.xlsx"), os.path.join(out_dir, "中文 文件名.xlsx")
    )
    dates_fixture(False).save(os.path.join(out_dir, "dates.xlsx"))
    dates_fixture(True).save(os.path.join(out_dir, "dates1904.xlsx"))
    rich_sst_fixture().save(os.path.join(out_dir, "rich_sst.xlsx"))
    ragged_fixture().save(os.path.join(out_dir, "ragged.xlsx"))
    no_rels_fixture().save(os.path.join(out_dir, "no_rels.xlsx"))

    bzip = basic_fixtures()
    bzip.save(os.path.join(out_dir, "bzip2.xlsx"), compression=zipfile.ZIP_BZIP2)

    with open(os.path.join(out_dir, "not_zip.xlsx"), "wb") as handle:
        handle.write(b"this is definitely not a zip archive\n")

    # ZIP64: force a zip64 local header on the worksheet entry.
    with zipfile.ZipFile(os.path.join(out_dir, "zip64.xlsx"), "w", zipfile.ZIP_DEFLATED) as archive:
        source = build_xlsx([("Zip64", sheet_xml(
            ['<row r="1"><c r="A1"><v>1</v></c></row>']))])
        for name, data in source.parts:
            info = zipfile.ZipInfo(name, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            if name.startswith("xl/worksheets/"):
                with archive.open(info, "w", force_zip64=True) as handle:
                    handle.write(data)
            else:
                archive.writestr(info, data)

    with open(os.path.join(out_dir, "basic.csv"), "wb") as handle:
        handle.write(
            b"id,name,note,amount,active\n"
            b"1,\xe5\xbc\xa0\xe4\xb8\x89,\"line1\nline2\",10.5,true\n"
            b"2,\"Bob, Jr.\",\"he said \"\"hi\"\"\",-2,false\n"
            b"3,Alice,,1e3,true\n"
        )

    content = (
        b"id,name,note,amount,active\n"
        b"1,\xe5\xbc\xa0\xe4\xb8\x89,\"line1\nline2\",10.5,true\n"
        b"2,\"Bob, Jr.\",\"he said \"\"hi\"\"\",-2,false\n"
        b"3,Alice,,1e3,true\n"
    )
    with open(os.path.join(out_dir, "bom_crlf.csv"), "wb") as handle:
        handle.write(b"\xef\xbb\xbf" + content.replace(b"\n", b"\r\n"))

    with open(os.path.join(out_dir, "basic.jsonl"), "wb") as handle:
        handle.write(
            b'{"id":1,"name":"\xe5\xbc\xa0\xe4\xb8\x89","note":"line1\\nline2","amount":10.5,'
            b'"active":true,"meta":{"nested":1}}\n'
            b'{"id":2,"name":"Bob, Jr.","note":"he said \\"hi\\"","amount":-2,"active":false,'
            b'"meta":null}\n'
            b'{"id":3,"name":"Alice","note":null,"amount":1000,"active":true,"meta":[1,2]}\n'
        )

    with open(os.path.join(out_dir, "unicode.jsonl"), "wb") as handle:
        handle.write(
            b'{"id":1,"emoji":"\\ud83d\\ude00","raw":"\xf0\x9f\x98\x80","control":"a\\tb\\u0000c"}\n'
        )

    with open(os.path.join(out_dir, "bad.jsonl"), "wb") as handle:
        handle.write(
            b'{"id":1,"name":"a"}\n{"id":2,"name":"b","extra":3}\n'
        )

    with open(os.path.join(out_dir, "bad.csv"), "wb") as handle:
        handle.write(b"id,name\n1,a,extra\n2,b\n")

    metadata_fixture().save(os.path.join(out_dir, "metadata.xlsx"))
    multi_fixture().save(os.path.join(out_dir, "multi.xlsx"))
    large_fixture().save(os.path.join(out_dir, "large.xlsx"))
    search_fixture().save(os.path.join(out_dir, "search.xlsx"))
    formulas_fixture().save(os.path.join(out_dir, "formulas.xlsx"))
    regions_fixture().save(os.path.join(out_dir, "regions.xlsx"))
    budget_fixture().save(os.path.join(out_dir, "budget.xlsx"))

    version = hashlib.sha256(open(__file__, "rb").read()).hexdigest()[:16]
    with open(os.path.join(out_dir, "VERSION"), "w", encoding="utf-8") as handle:
        handle.write(version + "\n")

    print("fixtures written to %s" % out_dir)


def make_big(path, rows, cols):
    out_dir = os.path.dirname(os.path.abspath(path))
    os.makedirs(out_dir, exist_ok=True)
    shared = ["name-%d" % index for index in range(min(rows, 1000))]
    sheet_rows = []
    header = "<row r=\"1\">" + "".join(
        '<c r="%s1" t="inlineStr"><is><t>c%d</t></is></c>' % (column_name(index), index)
        for index in range(1, cols + 1)
    ) + "</row>"
    sheet_rows.append(header)
    for row in range(2, rows + 1):
        cells = []
        cells.append('<c r="A%d"><v>%d</v></c>' % (row, row - 1))
        for col in range(2, cols + 1):
            if col == 2:
                cells.append(
                    '<c r="%s%d" t="s"><v>%d</v></c>'
                    % (column_name(col), row, (row - 2) % len(shared))
                )
            elif col == 3:
                cells.append('<c r="%s%d"><v>%.3f</v></c>' % (column_name(col), row, row * 0.5))
            elif col == 4:
                cells.append('<c r="%s%d" t="b"><v>%d</v></c>' % (column_name(col), row, row % 2))
            else:
                cells.append('<c r="%s%d"><v>%d</v></c>' % (column_name(col), row, row * col))
        sheet_rows.append('<row r="%d">%s</row>' % (row, "".join(cells)))
    build_xlsx([("Big", sheet_xml(sheet_rows))], shared_strings=shared).save(path)
    with open(os.path.splitext(path)[0] + ".csv", "w", encoding="utf-8", newline="") as handle:
        handle.write(",".join("c%d" % index for index in range(1, cols + 1)) + "\n")
        for row in range(1, rows):
            values = [str(row)]
            for col in range(2, cols + 1):
                values.append("%d" % (row * col))
            handle.write(",".join(values) + "\n")
    print("big fixtures written to %s" % out_dir)


def column_name(index):
    name = ""
    while index > 0:
        index, rem = divmod(index - 1, 26)
        name = chr(ord("A") + rem) + name
    return name


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="projects/tests/fixtures")
    parser.add_argument("--big", default=None)
    parser.add_argument("--rows", type=int, default=200000)
    parser.add_argument("--cols", type=int, default=8)
    args = parser.parse_args()
    if args.big:
        make_big(args.big, args.rows, args.cols)
    else:
        make_fixtures(args.out)


if __name__ == "__main__":
    sys.exit(main())
