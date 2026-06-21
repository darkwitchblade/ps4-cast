#!/usr/bin/env python3
# Generate app/src/web_ui.h from app/src/web_ui_src.html.
# The web control page is authored as a normal .html file (editable, lint-able,
# previewable) and embedded into the ELF as a C string by this generator, so we
# never hand-escape a large HTML/CSS/JS blob. Run from the repo root or app/.
import os, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
src = os.path.join(root, "app", "src", "web_ui_src.html")
dst = os.path.join(root, "app", "src", "web_ui.h")

with open(src, "r", encoding="utf-8") as f:
    text = f.read()

# Drop the trailing newline so we don't emit a final empty "" segment.
lines = text.split("\n")
if lines and lines[-1] == "":
    lines.pop()

out = []
out.append("// web_ui.h — GENERATED from web_ui_src.html by scripts/gen-web-ui.py.")
out.append("// Do NOT edit by hand: edit web_ui_src.html and re-run the generator")
out.append("// (the build does this automatically).")
out.append("#ifndef PS4CAST_WEB_UI_H")
out.append("#define PS4CAST_WEB_UI_H")
out.append("")
out.append("static const char WEB_UI_HTML[] =")
for ln in lines:
    esc = ln.replace("\\", "\\\\").replace('"', '\\"')
    out.append('"%s\\n"' % esc)
out.append(";")
out.append("")
out.append("#endif")

with open(dst, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")

# Report size of the served payload (bytes the browser receives).
payload = ("\n".join(lines) + "\n").encode("utf-8")
print("web_ui.h generated: %d source lines, %d bytes served" % (len(lines), len(payload)))
