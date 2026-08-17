#!/usr/bin/env python3
import os
import re
import base64
import gzip
import shutil

CLIENT_DIR = "client"
DIST_DIR = "client_dist"
INDEX_HTML = os.path.join(CLIENT_DIR, "index.html")
OUTPUT_GZ = os.path.join(DIST_DIR, "index.html.gz")

def read_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        return f.read()

def read_binary(filepath):
    with open(filepath, 'rb') as f:
        return f.read()

def get_base64_font(font_path):
    data = read_binary(font_path)
    b64 = base64.b64encode(data).decode('utf-8')
    return f"data:font/woff2;charset=utf-8;base64,{b64}"

def process_css(css_content):
    # Find url('../assets/...')
    def replace_url(match):
        url = match.group(1)
        if url.startswith('../assets/') and url.endswith('.woff2'):
            font_path = os.path.join(CLIENT_DIR, "assets", os.path.basename(url))
            if os.path.exists(font_path):
                return f"url('{get_base64_font(font_path)}')"
        return match.group(0)

    return re.sub(r"url\(['\"]?(.*?)['\"]?\)", replace_url, css_content)

def build():
    if not os.path.exists(DIST_DIR):
        os.makedirs(DIST_DIR)

    print("Building UI...")
    html = read_file(INDEX_HTML)

    # Inline CSS
    def css_replacer(match):
        css_file = match.group(1)
        filepath = os.path.join(CLIENT_DIR, css_file)
        print(f"  Inlining {css_file}")
        css = read_file(filepath)
        css = process_css(css)
        return f"<style>\n{css}\n</style>"

    html = re.sub(r'<link\s+rel="stylesheet"\s+href="(css/[^"]+)">', css_replacer, html)

    # Inline JS
    def js_replacer(match):
        js_file = match.group(1)
        filepath = os.path.join(CLIENT_DIR, js_file)
        print(f"  Inlining {js_file}")
        js = read_file(filepath)
        return f"<script>\n{js}\n</script>"

    html = re.sub(r'<script\s+src="(js/[^"]+)"></script>', js_replacer, html)

    # GZIP Compression
    print("Compressing...")
    with gzip.open(OUTPUT_GZ, 'wt', encoding='utf-8') as f:
        f.write(html)

    size = os.path.getsize(OUTPUT_GZ)
    print(f"Build complete: {OUTPUT_GZ} ({size} bytes)")

if __name__ == "__main__":
    build()
