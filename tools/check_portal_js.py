import re
import subprocess
import sys

p = r"C:\esp\RFID\main\portal_web.c"
t = open(p, encoding="utf-8").read()
m = re.search(
    r"static const char PORTAL_HTML\[\] =\s*(.*?);\s*\n\nesp_err_t portal_root",
    t,
    re.S,
)
if not m:
    sys.exit("PORTAL_HTML not found")
parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
html = "".join(parts)
s = re.search(r"<script>(.*?)</script>", html, re.S)
if not s:
    sys.exit("script not found")
js = s.group(1)
out = r"C:\esp\RFID\build\_portal.js"
open(out, "w", encoding="utf-8").write(js)
print("js len", len(js))
r = subprocess.run(["node", "--check", out], capture_output=True, text=True)
print("node exit", r.returncode)
if r.stderr:
    print(r.stderr)
if r.stdout:
    print(r.stdout)
