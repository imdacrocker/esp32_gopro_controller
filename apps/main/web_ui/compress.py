"""
compress.py <src_dir> <dst_dir>

Copies index.html and writes plain + gzipped versions of style.css / app.js /
updates.js into dst_dir. Called by CMake during `idf.py build`.

OTA-related runtime config (base URL, repo path) is exposed by the device
itself via /api/version — no need to bake another file into LittleFS.
"""
import sys
import gzip
import shutil
import os

if len(sys.argv) != 3:
    sys.exit("usage: compress.py <src_dir> <dst_dir>")

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)

shutil.copy2(os.path.join(src, 'index.html'), os.path.join(dst, 'index.html'))

for name in ('style.css', 'app.js', 'updates.js'):
    src_path = os.path.join(src, name)
    if not os.path.exists(src_path):
        continue
    data = open(src_path, 'rb').read()
    shutil.copy2(src_path, os.path.join(dst, name))
    with gzip.open(os.path.join(dst, name + '.gz'), 'wb', compresslevel=9) as f:
        f.write(data)
    ratio = 100 * (1 - len(gzip.compress(data, compresslevel=9)) / max(len(data), 1))
    print(f'  {name}: {len(data):,} B -> {name}.gz ({ratio:.0f}% smaller)')
