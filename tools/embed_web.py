#!/usr/bin/env python3
"""Embed main/web/index.html into a C header.

The HTML used to live as a C string literal in web_server.c, where a single
dropped quote silently truncated the CSS and served a broken page. Keeping the
markup in a real file and generating the header removes that whole class of bug.

Run automatically by the build; check in the generated header so a fresh clone
builds without Python.
"""

import gzip
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "web", "index.html")
OUT = os.path.join(ROOT, "main", "index_html.h")


def _byte_array(data, name):
    """Render bytes as a C array, 16 per line."""
    lines = ["static const char %s[] = {" % name]
    for i, b in enumerate(data):
        if i % 16 == 0:
            lines.append("    ")
        lines.append("0x%02x," % b)
        if i % 16 == 15:
            lines.append("")
    lines.append("    0x00")
    lines.append("};")
    return lines


def main():
    if not os.path.exists(SRC):
        sys.exit("missing %s" % SRC)

    with open(SRC, "r", encoding="utf-8") as fh:
        html = fh.read()
    raw_len = len(html.encode("utf-8"))
    data = _minify_html(html).encode("utf-8")

    # Compressed at build time, not on the device.
    #
    # Measured 2026-10-09: the link to the access point carries 50% loss at
    # 347 ms average RTT, which works out to roughly 400 bytes per second. The
    # 13 667 byte page needs about 35 s at that rate and in practice never
    # arrived at all. gzip takes the same page to 4 492 bytes - one third of the
    # traffic - because HTML of this shape is mostly repeated markup and CSS.
    #
    # The device has no business spending CPU on this: compressing once at build
    # time and serving a static blob is free at runtime, and every browser sends
    # Accept-Encoding: gzip anyway. The raw array stays in the header as the
    # fallback for clients that do not.
    packed = gzip.compress(data, 9)

    out = [
        "/* Generated from main/web/index.html by tools/embed_web.py.",
        " * Do not edit by hand; edit the HTML and rebuild. */",
        "#ifndef INDEX_HTML_H",
        "#define INDEX_HTML_H",
        "",
        "/* raw, for clients that do not accept gzip */",
        "#define INDEX_HTML_LEN %d" % len(data),
        "",
    ]
    out += _byte_array(data, "INDEX_HTML")
    out += [
        "",
        "/* gzip of the same bytes, served with Content-Encoding: gzip */",
        "#define INDEX_HTML_GZ_LEN %d" % len(packed),
        "",
    ]
    out += _byte_array(packed, "INDEX_HTML_GZ")
    out += [
        "",
        "#endif /* INDEX_HTML_H */",
        "",
    ]

    text = "\n".join(out)
    print("main/web/index.html %d B -> embedded %d B, gzip %d B (%.0f%%)"
          % (raw_len, len(data), len(packed), 100.0 * len(packed) / len(data)))
    existing = None
    if os.path.exists(OUT):
        with open(OUT, "r") as fh:
            existing = fh.read()
    if existing != text:
        with open(OUT, "w") as fh:
            fh.write(text)
        print("main/index_html.h regenerated (%d bytes)" % len(data))
    else:
        print("main/index_html.h already current")



# --------------------------------------------------------------------------
# Minification
#
# Measured: the device could not deliver the page over the air. Small responses
# were instant (185 B in 0.046 s) while every attempt at the 18.5 KB page stopped
# at exactly 8192 bytes with ESP_ERR_HTTPD_RESP_SEND and the browser saw an
# empty body. Heap was not the constraint (8.5 MB free), TX power made no
# difference across 8/13/17/19 dBm, the link never dropped, and the measured
# interrupt-masked window was 267 us as designed. Whatever the exact ceiling is,
# an 18.5 KB page does not fit this device's budget.
#
# The page is compressed on the way in; main/web/index.html stays readable and
# commented. The minifier is deliberately conservative - comments, indentation
# and whitespace between tags only. It does not rename identifiers or rewrite
# the CSS, because a silent mistake there is exactly the failure mode that once
# broke this stylesheet (a dropped quote).
# --------------------------------------------------------------------------

import re as _re

_CSS_COMMENT = _re.compile(r"/\*.*?\*/", _re.S)
_HTML_COMMENT = _re.compile(r"<!--.*?-->", _re.S)
_SPACE_BETWEEN_TAGS = _re.compile(r">\s+<")
_LINE_INDENT = _re.compile(r"^[ \t]+", _re.M)
_CSS_RULES = _re.compile(r"\{\s*([^}]*?)\s*\}")
_CSS_DECL_SPACE = _re.compile(r"\s*([:;,])\s*")
_CSS_WS_RUN = _re.compile(r"\s+")
_CSS_LAST_SEMICOLON = _re.compile(r";\s*\}")


def _minify_css(css):
    css = _CSS_COMMENT.sub("", css)
    css = _CSS_RULES.sub(
        lambda m: "{" + _CSS_DECL_SPACE.sub(r"\1", m.group(1)).strip() + "}",
        css)
    css = _CSS_LAST_SEMICOLON.sub("}", css)
    return _CSS_WS_RUN.sub(" ", css).strip()


_JS_BLOCK_COMMENT = _re.compile(r"/\*.*?\*/", _re.S)


def _minify_js(js):
    """Strip only what is provably safe.

    Comment removal is restricted to whole-line comments. A line-based rule
    cannot be fooled by "//" appearing inside a string literal, which is exactly
    where a naive _re.sub("//.*") would corrupt the program - the strings here
    are full of URLs. Newlines are kept throughout because JavaScript without a
    reliable ASI guarantee can change meaning if they are removed.
    """
    out = []
    i = 0
    lines = js.split("\n")
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        # A line comment on its own, possibly preceded by a block comment.
        if stripped.startswith("//"):
            i += 1
            continue
        # A block comment that occupies whole lines only.
        if stripped.startswith("/*"):
            end = line.find("*/")
            while end < 0 and i + 1 < len(lines):
                i += 1
                end = lines[i].find("*/")
            i += 1
            continue
        out.append(line)
        i += 1
    js = "\n".join(out)
    js = _JS_BLOCK_COMMENT.sub("", js)
    js = _re.sub(r"^[ \t]+", "", js, flags=_re.M)
    js = _re.sub(r"[ \t]{2,}", " ", js)
    return "\n".join(l for l in js.split("\n") if l.strip())


def _minify_html(html):
    out = _HTML_COMMENT.sub("", html)
    out = _LINE_INDENT.sub("", out)
    out = _SPACE_BETWEEN_TAGS.sub("><", out)
    out = _re.sub(r"<style>(.*?)</style>",
                  lambda m: "<style>" + _minify_css(m.group(1)) + "</style>",
                  out, flags=_re.S)
    out = _re.sub(r"<script>(.*?)</script>",
                  lambda m: "<script>" + _minify_js(m.group(1)) + "</script>",
                  out, flags=_re.S)
    return out.strip()


if __name__ == "__main__":
    main()
