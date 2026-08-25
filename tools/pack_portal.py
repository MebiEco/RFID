#!/usr/bin/env python3
"""Minify main/portal.html -> main/portal.html.gz for SoftAP embed."""
from pathlib import Path
import gzip
import re

ROOT = Path(__file__).resolve().parents[1]
src = ROOT / "main" / "portal.html"
dst = ROOT / "main" / "portal.html.gz"

html = src.read_text(encoding="utf-8")
html = re.sub(r">\s+<", "><", html).strip()


def min_css(m):
    css = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    css = re.sub(r"\s+", " ", css)
    for a, b in ((" {", "{"), ("{ ", "{"), (" }", "}"), ("} ", "}"), (": ", ":"), ("; ", ";")):
        css = css.replace(a, b)
    return "<style>" + css.strip() + "</style>"


html = re.sub(r"<style>(.*?)</style>", min_css, html, flags=re.S)


def min_js(m):
    js = m.group(1)
    lines = [ln.strip() for ln in js.splitlines() if ln.strip()]
    js = "".join(lines)
    js = re.sub(r" {2,}", " ", js)
    return "<script>" + js + "</script>"


html = re.sub(r"<script>(.*?)</script>", min_js, html, flags=re.S)
raw = html.encode("utf-8")
gz = gzip.compress(raw, compresslevel=9)
dst.write_bytes(gz)
print(f"portal.html {len(raw)} B -> portal.html.gz {len(gz)} B ({100 * len(gz) / len(raw):.1f}%)")
