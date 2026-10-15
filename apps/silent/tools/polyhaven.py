"""Downloads from Poly Haven and ambientCG, cached, for the tools that take their CC0 scans.

The standard library only, so Blender's own Python can import it too. Downloads are cached under
out/polyhaven_cache, the build tree, gitignored.
"""

import json
import os
import re
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CACHE_DIR = os.path.join(ROOT, "out", "polyhaven_cache")
API = "https://api.polyhaven.com/files/%s"
INFO = "https://api.polyhaven.com/info/%s"


def cached_path(url, cache_name=None):
    """Where `fetch` keeps `url`'s bytes: under `cache_name`, or the URL's last part with anything
    a file name cannot hold on every platform replaced (ambientCG's end in `get?file=`)."""
    return os.path.join(CACHE_DIR,
                        cache_name or re.sub(r'[<>:"/\\|?*]', "_", url.rsplit("/", 1)[-1]))


def fetched(url, cache_name=None):
    """Where `url`'s bytes are kept, downloaded first when they are not there yet."""
    path = cached_path(url, cache_name)
    if not os.path.exists(path):
        os.makedirs(CACHE_DIR, exist_ok=True)
        req = urllib.request.Request(url, headers={"User-Agent": "cetra-silent/1.0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    return path


def fetch(url, cache_name=None):
    """The bytes at `url`, from the cache when they are there."""
    with open(fetched(url, cache_name), "rb") as f:
        return f.read()


def polyhaven_urls(name, keys, resolution="1k"):
    """A Poly Haven set's JPEG maps' URLs by the caller's names for Poly Haven's keys
    (`{"albedo": "Diffuse", ...}`), and the set's info: what was scanned, and how large."""
    files = json.loads(fetch(API % name, name + "_files.json"))
    info = json.loads(fetch(INFO % name, name + "_info.json"))
    return {kind: files[key][resolution]["jpg"]["url"] for kind, key in keys.items()}, info
