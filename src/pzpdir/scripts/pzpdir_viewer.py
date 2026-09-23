#!/usr/bin/env python3
"""
pzpdir_viewer.py - A small wxPython explorer for .pzpd archives.

Opens an archive (manifest, single shard, or collection) and shows:
  - every record (ordinal, key, member), filtered by a key substring, or by a case-insensitive
    substring of the record's text table columns (e.g. descriptions; indexed on first use);
  - for the selected record: every stream's blob (name, format, WxHxC@bits, size) and every
    table's rows (captions, image size, persons, descriptors as length + norm + first values);
  - a preview of one stream: images decoded (PZP natively, others through PIL), 16-bit and
    single-channel data stretched to 8 bits for display, persons (bbox + keypoints) drawn on top
    when the record has a `persons` table; text blobs shown as text; a 3-channel 8-bit PZP image
    can also be viewed as its segmentation (channel 0) or its 16-bit depth (channels 1+2), the
    layout of the combined label + depth files;
  - an archive summary: members, shards (storage, recovery, AUTO prefetch mode), streams, tables;
  - the record's histograms (hist_rgb / hist_seg / hist_depth tables, spec §3.8) as bars over the
    archive-wide ones as a line.
The selected blob can be saved to a file.

Usage:
    python3 pzpdir_viewer.py [archive.pzpd]

Needs wxPython, numpy (>= 2.0, for the search indexes), PIL, and the PZP Python package (pzp.pzpdir); when that isn't installed
it is taken from this repository's src/ directory.

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import os
import sys

import numpy as np
import wx

try:
    import pzp.pzpdir as pzpdir
except ImportError:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    import pzp.pzpdir as pzpdir

IMAGE_FORMATS = {"JPEG", "PNG", "PZP", "PZPC", "PNM", "PFM"}
TEXT_FORMATS = {"JSON", "TEXT", "CSV", "TSV"}

# One colour per segmentation label; label 0 (background) stays black
LABEL_COLOURS = np.random.RandomState(7).randint(60, 256, (256, 3)).astype(np.uint8)
LABEL_COLOURS[0] = 0
SEGMENTATION_VIEW = "Segmentation (channel 0)"
DEPTH_VIEW = "Depth (channels 1+2, 16-bit)"


def image_views(arr, fmt):
    """
    The views of a decoded image the preview offers.

    Parameters
    ----------
    arr : numpy.ndarray
        A decoded image, as read_image returns it.
    fmt : str
        The blob's format ("PZP", "PZPC", "JPEG", ...).

    Returns
    -------
    dict
        view name -> image. Always "Image" (arr itself); a 3-channel 8-bit PZP image may be a
        combined label + depth file (channel 0 = segmentation labels, channels 1 and 2 = high and
        low byte of a 16-bit depth), so it also gets those two as standalone images.
    """
    views = {"Image": arr}
    if fmt in ("PZP", "PZPC") and arr.ndim == 3 and arr.shape[2] == 3 and arr.dtype == np.uint8:
        views[SEGMENTATION_VIEW] = arr[:, :, 0]
        views[DEPTH_VIEW] = (arr[:, :, 1].astype(np.uint16) << 8) | arr[:, :, 2]
    return views


def to_display(arr):
    """
    Convert a decoded image to 8-bit RGB for display.

    Parameters
    ----------
    arr : numpy.ndarray
        HxW or HxWxC, uint8 / uint16 / float.

    Returns
    -------
    numpy.ndarray
        HxWx3 uint8 (16-bit, float and single-channel data are stretched to their min..max).
    """
    if arr.ndim == 2:
        arr = arr[:, :, None]
    if arr.shape[2] == 2 or arr.shape[2] > 4:
        arr = arr[:, :, :1]
    if arr.shape[2] == 4:
        arr = arr[:, :, :3]
    if arr.dtype != np.uint8:
        a = arr.astype(np.float64)
        lo, hi = float(a.min()), float(a.max())
        arr = np.zeros(a.shape, np.uint8) if hi <= lo else ((a - lo) * (255.0 / (hi - lo))).astype(np.uint8)
    if arr.shape[2] == 1:
        arr = np.repeat(arr, 3, axis=2)
    return np.ascontiguousarray(arr)


def build_key_index(archive):
    """
    Every record key as one numpy string array ( numpy >= 2 StringDType ), so a key search is a
    vectorised np.strings.find instead of one library call per record.
    """
    return np.array([archive.key(o) for o in range(len(archive))], dtype=np.dtypes.StringDType())


def build_text_index(archive):
    """
    Lower-cased text of every record's string table columns, for case-insensitive search.

    Parameters
    ----------
    archive : pzpdir.Archive
        The opened archive.

    Returns
    -------
    list of (numpy.ndarray, numpy.ndarray)
        One (owners, texts) pair per str column of every record table: texts[r] is row r's value
        lower-cased ( numpy StringDType ), owners[r] the ordinal of the record the row belongs to.
    """
    columns = []
    for t in archive.tables:
        sc = archive.schema(t)
        cols = [name for name, ty, count, _off in sc["columns"] if ty == "str" and count == 1]
        if sc["global"] or not cols:
            continue
        index, rows = archive.table_all(t)   # one bulk read per table
        owners = np.repeat(np.arange(len(archive)), np.diff(index))
        for name in cols:
            columns.append((owners, np.strings.lower(rows[name].astype(np.dtypes.StringDType()))))
    return columns


def search_text_index(columns, needle):
    """Sorted ordinals of the records with a row whose text contains needle ( already lower-cased )."""
    hits = [owners[np.strings.find(texts, needle) >= 0] for owners, texts in columns]
    return np.unique(np.concatenate(hits)) if hits else np.zeros(0, dtype=np.int64)


def histogram_kinds(archive):
    """Kinds with a per-file histogram table `hist_<kind>` (record table `h:u16[256]`, spec §3.8), in table order."""
    kinds = []
    for t in archive.tables:
        if t.startswith("hist_") and not t.endswith("_global"):
            sc = archive.schema(t)
            if not sc["global"] and [(n, ty, c) for n, ty, c, _o in sc["columns"]] == [("h", "u16", 256)]:
                kinds.append(t[5:])
    return kinds


def global_histogram(archive, kind):
    """
    The archive-wide histogram of a kind as fractions (numpy float, 256), or None without `hist_<kind>_global`.
    Several members are combined pixel-weighted with their `pixels` totals.
    """
    name = "hist_%s_global" % kind
    if name not in archive.tables:
        return None
    acc, pixels = np.zeros(256), 0
    for m in range(len(archive.members)):
        try:
            rows = archive.global_table(name, member=m)
        except pzpdir.PzpdError:          # a missing member contributes nothing
            continue
        for r in rows:
            acc += r["h"].astype(np.float64) / 65535.0 * int(r["pixels"])
            pixels += int(r["pixels"])
    return acc / pixels if pixels else None


class HistogramPanel(wx.Panel):
    """The selected record's histograms (bars) over the archive-wide ones (line), one band per kind (spec §3.8)."""

    TITLES = {"rgb": "luminance", "seg": "segmentation labels", "depth": "depth (bin = depth >> 8)"}
    COLOURS = {"rgb": wx.Colour(210, 210, 210), "seg": wx.Colour(120, 205, 120), "depth": wx.Colour(110, 160, 255)}

    def __init__(self, parent):
        super().__init__(parent, style=wx.FULL_REPAINT_ON_RESIZE)
        self.SetBackgroundStyle(wx.BG_STYLE_PAINT)
        self.bands, self.message = [], ""
        self.Bind(wx.EVT_PAINT, self.on_paint)

    def show(self, bands, message=""):
        """bands: [(kind, this file's fractions or None, archive-wide fractions or None)]."""
        self.bands, self.message = bands, message
        self.Refresh()

    def on_paint(self, _evt):
        dc = wx.AutoBufferedPaintDC(self)
        dc.SetBackground(wx.Brush(wx.Colour(40, 40, 40)))
        dc.Clear()
        dc.SetTextForeground(wx.Colour(220, 220, 220))
        dc.SetFont(wx.Font(wx.FontInfo(8)))
        W, H = self.GetClientSize()
        if not self.bands:
            dc.DrawText(self.message, 10, 10)
            return
        bh = H // len(self.bands)
        for b, (kind, fh, gh) in enumerate(self.bands):
            top, left, right = b * bh, 50, W - 10
            pw, ph = max(1, right - left), max(1, bh - 34)
            y0 = top + 18 + ph                          # the plot's baseline
            ymax = max([float(x.max()) for x in (fh, gh) if x is not None] + [1e-6])
            colour = self.COLOURS.get(kind, wx.Colour(230, 180, 90))
            what = "this file (bars)" if fh is not None else "this file: none"
            dc.DrawText("%s  -  %s, archive (line)%s   peak %.1f%%" % (self.TITLES.get(kind, kind), what, "" if gh is not None else ": none", 100 * ymax), left, top + 2)
            dc.SetPen(wx.Pen(wx.Colour(90, 90, 90)))
            dc.DrawLine(left, y0, right, y0)
            for v in (0, 64, 128, 192, 255):            # bin ticks
                x = left + int((v + 0.5) * pw / 256)
                dc.DrawLine(x, y0, x, y0 + 3)
                tw = dc.GetTextExtent(str(v))[0]
                dc.DrawText(str(v), min(x - tw // 2, right - tw), y0 + 3)
            dc.DrawText("%.0f%%" % (100 * ymax), 4, top + 16)
            if fh is not None:
                dc.SetPen(wx.TRANSPARENT_PEN)
                dc.SetBrush(wx.Brush(colour))
                for i in range(256):
                    h = int(round(fh[i] / ymax * ph))
                    if h > 0:
                        x0, x1 = left + i * pw // 256, left + (i + 1) * pw // 256
                        dc.DrawRectangle(x0, y0 - h, max(1, x1 - x0), h)
            if gh is not None:
                dc.SetPen(wx.Pen(wx.Colour(255, 120, 60), 2))
                dc.DrawLines([wx.Point(left + int((i + 0.5) * pw / 256), y0 - int(round(gh[i] / ymax * ph))) for i in range(256)])
            if kind == "seg" and fh is not None:        # which labels the file holds most
                top3 = [i for i in np.argsort(-fh)[:3] if fh[i] > 0]
                dc.DrawText("labels: " + "  ".join("%d %.1f%%" % (i, 100 * fh[i]) for i in top3), right - 260, top + 2 + 14)


class RecordList(wx.ListCtrl):
    """Virtual list of the (filtered) records: ordinal, key, member."""

    def __init__(self, parent):
        super().__init__(parent, style=wx.LC_REPORT | wx.LC_VIRTUAL | wx.LC_SINGLE_SEL)
        self.InsertColumn(0, "#", width=70)
        self.InsertColumn(1, "key", width=260)
        self.InsertColumn(2, "member", width=90)
        self.archive, self.ordinals = None, []

    def set_records(self, archive, ordinals):
        self.archive, self.ordinals = archive, ordinals
        self.SetItemCount(len(ordinals))
        self.Refresh()

    def OnGetItemText(self, item, col):
        o = self.ordinals[item]
        if col == 0:
            return str(o)
        if col == 1:
            return self.archive.key(o)
        return self.archive.member_of(o)[0]


class Preview(wx.Panel):
    """Draws one decoded image scaled to fit, with an optional persons overlay."""

    def __init__(self, parent):
        super().__init__(parent, style=wx.FULL_REPAINT_ON_RESIZE)
        self.SetBackgroundStyle(wx.BG_STYLE_PAINT)
        self.image, self.persons, self.size, self.message = None, [], None, "Open an archive (File > Open)"
        self.Bind(wx.EVT_PAINT, self.on_paint)

    def show(self, rgb=None, persons=(), size=None, message=""):
        """rgb: HxWx3 uint8 or None; persons: rows with bbox / kp in `size` (w, h) coordinates."""
        self.image = wx.Image(rgb.shape[1], rgb.shape[0], rgb.tobytes()) if rgb is not None else None
        self.persons, self.size, self.message = list(persons), size, message
        self.Refresh()

    def on_paint(self, _evt):
        dc = wx.AutoBufferedPaintDC(self)
        dc.SetBackground(wx.Brush(wx.Colour(40, 40, 40)))
        dc.Clear()
        W, H = self.GetClientSize()
        if self.image is None:
            dc.SetTextForeground(wx.Colour(220, 220, 220))
            dc.DrawText(self.message, 10, 10)
            return
        iw, ih = self.image.GetWidth(), self.image.GetHeight()
        s = min(W / iw, H / ih)
        dw, dh = max(1, int(iw * s)), max(1, int(ih * s))
        ox, oy = (W - dw) // 2, (H - dh) // 2
        dc.DrawBitmap(wx.Bitmap(self.image.Scale(dw, dh, wx.IMAGE_QUALITY_BILINEAR)), ox, oy)
        if not self.persons:
            return
        # Keypoints / bboxes are in the image table's size (the original image); scale to the preview
        sx = dw / float(self.size[0] or iw)
        sy = dh / float(self.size[1] or ih)
        colours = [wx.Colour(255, 80, 80), wx.Colour(80, 255, 80), wx.Colour(80, 160, 255), wx.Colour(255, 220, 60)]
        for n, p in enumerate(self.persons):
            c = colours[n % len(colours)]
            dc.SetPen(wx.Pen(c, 2))
            dc.SetBrush(wx.TRANSPARENT_BRUSH)
            x1, y1, x2, y2 = [int(v) for v in p["bbox"]]   # corners, as the .db files store them
            dc.DrawRectangle(ox + int(x1 * sx), oy + int(y1 * sy), max(1, int((x2 - x1) * sx)), max(1, int((y2 - y1) * sy)))
            dc.SetBrush(wx.Brush(c))
            kp = p["kp"]
            for j in range(0, len(kp) - 2, 3):
                if kp[j + 2] > 0:
                    dc.DrawCircle(ox + int(kp[j] * sx), oy + int(kp[j + 1] * sy), 3)


class ViewerFrame(wx.Frame):
    def __init__(self, path=None):
        super().__init__(None, title="pzpdir viewer", size=(1280, 820))
        self.archive, self.path, self.current = None, None, None
        self.key_index = None    # build_key_index() of the open archive, built on the first key search
        self.text_index = None   # build_text_index() of the open archive, built on the first text search

        menu = wx.Menu()
        self.Bind(wx.EVT_MENU, self.on_open, menu.Append(wx.ID_OPEN, "&Open...\tCtrl+O"))
        self.Bind(wx.EVT_MENU, self.on_save_blob, menu.Append(wx.ID_SAVEAS, "&Save blob...\tCtrl+S"))
        menu.AppendSeparator()
        self.Bind(wx.EVT_MENU, lambda _e: self.Close(), menu.Append(wx.ID_EXIT, "&Quit\tCtrl+Q"))
        bar = wx.MenuBar()
        bar.Append(menu, "&File")
        self.SetMenuBar(bar)
        self.CreateStatusBar()

        split = wx.SplitterWindow(self, style=wx.SP_LIVE_UPDATE)
        left = wx.Panel(split)
        self.search = wx.SearchCtrl(left, style=wx.TE_PROCESS_ENTER)
        self.search.SetDescriptiveText("filter keys")
        self.scope = wx.Choice(left, choices=["key", "text tables"])
        self.scope.SetSelection(0)
        self.scope.SetToolTip("key: substring of the record key\n"
                              "text tables: case-insensitive substring of the record's text columns (e.g. descriptions)")
        self.records = RecordList(left)
        ss = wx.BoxSizer(wx.HORIZONTAL)
        ss.Add(self.search, 1, wx.EXPAND | wx.RIGHT, 4)
        ss.Add(self.scope, 0, wx.ALIGN_CENTER_VERTICAL)
        ls = wx.BoxSizer(wx.VERTICAL)
        ls.Add(ss, 0, wx.EXPAND | wx.ALL, 4)
        ls.Add(self.records, 1, wx.EXPAND)
        left.SetSizer(ls)

        right = wx.Panel(split)
        top = wx.BoxSizer(wx.HORIZONTAL)
        top.Add(wx.StaticText(right, label="Stream:"), 0, wx.ALIGN_CENTER_VERTICAL | wx.ALL, 4)
        self.stream = wx.Choice(right)
        top.Add(self.stream, 0, wx.ALL, 4)
        top.Add(wx.StaticText(right, label="View:"), 0, wx.ALIGN_CENTER_VERTICAL | wx.ALL, 4)
        self.view = wx.Choice(right, choices=["Image"])
        self.view.SetSelection(0)
        top.Add(self.view, 0, wx.ALL, 4)
        self.overlay = wx.CheckBox(right, label="Draw persons")
        self.overlay.SetValue(True)
        top.Add(self.overlay, 0, wx.ALIGN_CENTER_VERTICAL | wx.ALL, 4)
        rsplit = wx.SplitterWindow(right, style=wx.SP_LIVE_UPDATE)
        self.preview = Preview(rsplit)
        self.tabs = wx.Notebook(rsplit)
        mono = wx.Font(wx.FontInfo(9).Family(wx.FONTFAMILY_TELETYPE))
        self.recordText = wx.TextCtrl(self.tabs, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.HSCROLL)
        self.archiveText = wx.TextCtrl(self.tabs, style=wx.TE_MULTILINE | wx.TE_READONLY | wx.HSCROLL)
        self.recordText.SetFont(mono)
        self.archiveText.SetFont(mono)
        self.hist = HistogramPanel(self.tabs)
        self.tabs.AddPage(self.recordText, "Record")
        self.tabs.AddPage(self.archiveText, "Archive")
        self.tabs.AddPage(self.hist, "Histograms")
        self.hist_kinds, self.hist_global = [], {}
        rsplit.SplitHorizontally(self.preview, self.tabs, 470)
        rsplit.SetMinimumPaneSize(80)
        rs = wx.BoxSizer(wx.VERTICAL)
        rs.Add(top, 0, wx.EXPAND)
        rs.Add(rsplit, 1, wx.EXPAND)
        right.SetSizer(rs)
        split.SplitVertically(left, right, 430)
        split.SetMinimumPaneSize(200)

        self.search.Bind(wx.EVT_TEXT_ENTER, self.on_filter)
        self.search.Bind(wx.EVT_SEARCHCTRL_SEARCH_BTN, self.on_filter)
        self.search.Bind(wx.EVT_SEARCHCTRL_CANCEL_BTN, self.on_clear_filter)
        self.scope.Bind(wx.EVT_CHOICE, self.on_scope)
        self.records.Bind(wx.EVT_LIST_ITEM_SELECTED, self.on_select)
        self.stream.Bind(wx.EVT_CHOICE, lambda _e: self.show_preview())
        self.view.Bind(wx.EVT_CHOICE, lambda _e: self.show_preview())
        self.overlay.Bind(wx.EVT_CHECKBOX, lambda _e: self.show_preview())
        self.Bind(wx.EVT_CLOSE, self.on_close)
        if path:
            self.load(path)

    # --- archive ----------------------------------------------------------------------------
    def load(self, path):
        try:
            a = pzpdir.open(path, allow_missing=True)
        except pzpdir.PzpdError as e:
            wx.MessageBox("Cannot open %s:\n%s" % (path, e), "pzpdir viewer", wx.ICON_ERROR)
            return False
        if self.archive is not None:
            self.archive.close()
        self.archive, self.path = a, path
        self.key_index = self.text_index = None
        self.SetTitle("pzpdir viewer - %s" % path)
        self.search.SetValue("")
        self.stream.SetItems(a.streams)
        if a.streams:
            self.stream.SetSelection(0)
        self.records.set_records(a, list(range(len(a))))
        has_persons = "persons" in a.tables   # an archive packed without annotations has nothing to draw
        self.overlay.Enable(has_persons)
        if has_persons:
            self.overlay.UnsetToolTip()
        else:
            self.overlay.SetToolTip("this archive has no persons table")
        self.archiveText.SetValue(self.archive_summary())
        self.hist_kinds = histogram_kinds(a)
        self.hist_global = {k: global_histogram(a, k) for k in self.hist_kinds}
        self.SetStatusText("%d records, %d streams, %d tables" % (len(a), len(a.streams), len(a.tables)))
        if len(a):
            self.records.Select(0)
            self.records.Focus(0)
        return True

    def archive_summary(self):
        a = self.archive
        out = ["Archive  %s" % self.path, "Records  %d" % len(a), ""]
        out.append("Members")
        for m, alias in enumerate(a.members):
            first, count = a.member_range(m)
            out.append("  %-3d %-24s ordinals %d..%d (%d)" % (m, alias, first, first + count - 1, count))
        out += ["", "Shards"]
        for s in a.shards():
            out.append("  %-60s member %d  records %-7d %8.1f MB  %s%s  AUTO -> %s" % (
                s.get("path", "?"), s.get("member", 0), s.get("record_count", 0), s.get("file_bytes", 0) / 1e6,
                s.get("storage", "?"), "" if not s.get("recovery") else "  (recovery %d)" % s["recovery"],
                s.get("auto_mode", "?")))
        out += ["", "Streams"]
        for st in a.streams:
            out.append("  %s" % st)
        out += ["", "Tables"]
        for t in a.tables:
            sc = a.schema(t)
            cols = " ".join("%s:%s%s" % (n, ty, "" if c == 1 else "[%d]" % c) for n, ty, c, _o in sc["columns"])
            kind = "global" if sc["global"] else ("record, bulk" if sc["bulk"] else "record")
            out.append("  %-20s %-14s %s" % (t, "(" + kind + ")", cols))
        return "\n".join(out)

    # --- records ----------------------------------------------------------------------------
    def on_filter(self, _evt):
        if self.archive is None:
            return
        text = self.search.GetValue()
        a = self.archive
        in_text = self.scope.GetSelection() == 1
        wx.BeginBusyCursor()
        try:
            if not text:
                ordinals = list(range(len(a)))
            elif in_text:
                if self.text_index is None:
                    self.text_index = build_text_index(a)
                ordinals = search_text_index(self.text_index, text.lower()).tolist()
            else:
                if self.key_index is None:
                    self.key_index = build_key_index(a)
                ordinals = np.flatnonzero(np.strings.find(self.key_index, text) >= 0).tolist()
        finally:
            wx.EndBusyCursor()
        self.records.set_records(a, ordinals)
        self.SetStatusText("%d of %d records match %r%s" % (len(ordinals), len(a), text, " in text tables" if in_text else ""))
        if ordinals:
            self.records.Select(0)

    def on_scope(self, _evt):
        self.search.SetDescriptiveText("filter keys" if self.scope.GetSelection() == 0 else "filter text (any case)")
        if self.search.GetValue():
            self.on_filter(None)

    def on_clear_filter(self, _evt):
        self.search.SetValue("")
        self.on_filter(None)

    def on_select(self, evt):
        self.current = self.records.ordinals[evt.GetIndex()]
        self.recordText.SetValue(self.record_summary(self.current))
        self.show_histograms()
        self.show_preview()

    def show_histograms(self):
        a, o = self.archive, self.current
        if not self.hist_kinds:
            self.hist.show([], "this archive has no histogram tables (add them with scripts/pzpdir_histograms.py)")
            return
        bands = []
        for k in self.hist_kinds:
            rows = a.table(o, "hist_" + k)
            bands.append((k, rows["h"][0].astype(np.float64) / 65535.0 if len(rows) else None, self.hist_global.get(k)))
        self.hist.show(bands)

    def record_summary(self, o):
        a = self.archive
        alias, local = a.member_of(o)
        out = ["Record %d   key %r   member %s (record %d there)" % (o, a.key(o), alias, local)]
        g = a.group(o)
        if g is not None:
            out.append("Group %r  frame %d of %d" % (g.get("name", ""), g.get("index", 0), g.get("frames", 0)))
        out += ["", "Blobs"]
        for st in a.streams:
            inf = a.info(o, st)
            if inf is None:
                out.append("  %-8s -" % st)
                continue
            out.append("  %-8s %-5s %5dx%-5d x%d @%2d bit  %10d bytes  %s" % (
                st, inf["format"].strip(), inf["width"], inf["height"], inf["channels"], inf["bits"], inf["size"], inf["name"]))
        for t in a.tables:
            sc = a.schema(t)
            if sc["global"]:
                continue
            rows = a.table(o, t)
            out += ["", "%s (%d rows)" % (t, len(rows))]
            for r in rows[:50]:
                fields = []
                for name, ty, count, _off in sc["columns"]:
                    v = r[name]
                    if ty == "str":
                        fields.append("%s=%r" % (name, v))
                    elif count > 16 and ty in ("f32", "f64"):
                        arr = np.asarray(v, dtype=np.float64)
                        fields.append("%s=[%d values, norm %.4f: %s ...]" % (name, count, float(np.linalg.norm(arr)),
                                                                          ", ".join("%.4g" % x for x in arr[:6])))
                    elif count > 16:                   # long integer arrays, e.g. histograms (Histograms tab)
                        arr = np.asarray(v, dtype=np.int64)
                        fields.append("%s=[%d values, sum %d: %s ...]" % (name, count, int(arr.sum()), ", ".join(str(x) for x in arr[:8])))
                    else:
                        fields.append("%s=%s" % (name, np.asarray(v).tolist()))
                out.append("  " + "  ".join(fields))
            if len(rows) > 50:
                out.append("  ... %d more" % (len(rows) - 50))
        return "\n".join(out)

    def show_preview(self):
        a, o = self.archive, self.current
        if a is None or o is None or self.stream.GetSelection() < 0:
            return
        st = a.streams[self.stream.GetSelection()]
        inf = a.info(o, st)
        self.view.Disable()   # enabled again below once an image with several views is decoded
        if inf is None:
            self.preview.show(message="record %d has no %s blob" % (o, st))
            return
        fmt = inf["format"].strip()
        if fmt in TEXT_FORMATS:
            text = a.read(o, st)[:4000].decode("utf-8", "replace")
            self.preview.show(message=text.replace("\t", "    ").split("\n")[0][:200])
            self.recordText.SetValue(self.record_summary(o) + "\n\n%s blob (first 4000 bytes)\n%s" % (st, text))
            return
        if fmt not in IMAGE_FORMATS:
            self.preview.show(message="no preview for %s blobs (%d bytes)" % (fmt, inf["size"]))
            return
        try:
            views = image_views(a.read_image(o, st), fmt)
        except Exception as e:   # a damaged or unsupported blob must not take the viewer down
            self.preview.show(message="cannot decode %s: %s" % (inf["name"], e))
            return
        names = list(views)
        if self.view.GetItems() != names:   # keep the chosen view while it is on offer
            keep = self.view.GetStringSelection()
            self.view.SetItems(names)
            self.view.SetSelection(names.index(keep) if keep in names else 0)
        self.view.Enable(len(names) > 1)
        view = self.view.GetStringSelection()
        data = views[view]
        if view == SEGMENTATION_VIEW:
            rgb = np.ascontiguousarray(LABEL_COLOURS[data])
            detail = "  %s: %d labels" % (view, len(np.unique(data)))
        elif view == DEPTH_VIEW:
            rgb = to_display(data)
            detail = "  %s: %d..%d" % (view, int(data.min()), int(data.max()))
        else:
            rgb = to_display(data)
            detail = ""
        persons, size = [], None
        if self.overlay.GetValue() and "persons" in a.tables:
            persons = list(a.table(o, "persons"))
            if "image" in a.tables and len(a.table(o, "image")):
                r = a.table(o, "image")[0]
                size = (int(r["width"]), int(r["height"]))
            else:
                size = (rgb.shape[1], rgb.shape[0])
        self.preview.show(rgb, persons, size)
        self.SetStatusText("%s  %s  %dx%d x%d @%d bit%s" % (a.key(o), st, inf["width"], inf["height"], inf["channels"], inf["bits"], detail))

    # --- menu ---------------------------------------------------------------------------------
    def on_open(self, _evt):
        with wx.FileDialog(self, "Open archive", wildcard="PZPD archives (*.pzpd)|*.pzpd|All files|*",
                           style=wx.FD_OPEN | wx.FD_FILE_MUST_EXIST) as dlg:
            if dlg.ShowModal() == wx.ID_OK:
                self.load(dlg.GetPath())

    def on_save_blob(self, _evt):
        a, o = self.archive, self.current
        if a is None or o is None or self.stream.GetSelection() < 0:
            return
        st = a.streams[self.stream.GetSelection()]
        inf = a.info(o, st)
        if inf is None:
            return
        with wx.FileDialog(self, "Save %s blob" % st, defaultFile=os.path.basename(inf["name"]),
                           style=wx.FD_SAVE | wx.FD_OVERWRITE_PROMPT) as dlg:
            if dlg.ShowModal() == wx.ID_OK:
                with open(dlg.GetPath(), "wb") as f:
                    f.write(a.read(o, st))
                self.SetStatusText("saved %s (%d bytes)" % (dlg.GetPath(), inf["size"]))

    def on_close(self, evt):
        if self.archive is not None:
            self.archive.close()
            self.archive = None
        evt.Skip()


def main():
    app = wx.App()
    ViewerFrame(sys.argv[1] if len(sys.argv) > 1 else None).Show()
    app.MainLoop()


if __name__ == "__main__":
    main()
