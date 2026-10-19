#!/usr/bin/env python3
"""Pack Orca's resources/ directory into one zip that ships as an APK asset (unpacked at first run).

usage: make_resources_zip.py <resources_dir> <out.zip>
The zip carries a ".orca-stamp" entry (hash of file names + sizes) so the app knows when to re-extract.
"""
import hashlib, os, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
# desktop-only payloads that make no sense on a phone
skip_top = {"tools", "applescript"}
entries = []
for root, dirs, files in os.walk(src):
    rel_root = os.path.relpath(root, src)
    if rel_root == ".":
        dirs[:] = [d for d in dirs if d not in skip_top]
    dirs.sort()
    for f in sorted(files):
        p = os.path.join(root, f)
        entries.append((os.path.relpath(p, src), p))
h = hashlib.sha1()
for rel, p in entries:
    h.update(rel.encode()); h.update(str(os.path.getsize(p)).encode())
os.makedirs(os.path.dirname(out), exist_ok=True)
tmp = out + ".tmp"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    z.writestr(".orca-stamp", h.hexdigest())
    for rel, p in entries:
        z.write(p, rel)
os.replace(tmp, out)
print("resources.zip:", len(entries), "files, stamp", h.hexdigest())
