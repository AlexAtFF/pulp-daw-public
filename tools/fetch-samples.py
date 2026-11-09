#!/usr/bin/env python3
"""Fetch the PULP factory sample sources into the download cache.

Reads  factory/sources.json  (the manifest: every file's URL, cache path,
SHA-256 and size) and downloads each file into  factory-cache/  beside
factory/.  Standard library only.  Run from the project folder:

    python -I tools\\fetch-samples.py            download what is missing
    python -I tools\\fetch-samples.py --check    verify the cache, no network

Behaviour
  * A file already in the cache with the right SHA-256 is skipped.
  * Everything downloaded is hashed while it streams to a ".part" file and
    only renamed into place when size and SHA-256 match the manifest.
  * A failed download (network error or wrong checksum) is retried twice.
  * Any file that still fails is listed on stderr and the exit code is 1.
  * Only https URLs are fetched; nothing is executed or unpacked.

The cache is not part of the source tree.  Licences and credits for the
files are in factory/SOURCES.md and in the manifest itself.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

RETRIES = 2            # extra attempts after the first one
TIMEOUT = 60           # seconds, per connection / read
CHUNK = 1 << 16
USER_AGENT = "pulp-fetch-samples/1 (+factory/sources.json)"

_print_lock = threading.Lock()


def say(text, err=False):
    with _print_lock:
        stream = sys.stderr if err else sys.stdout
        stream.write(text.encode("ascii", "replace").decode("ascii") + "\n")
        stream.flush()


def sha256_of(path):
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            h.update(block)
            size += len(block)
    return h.hexdigest(), size


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    if doc.get("format") != "pulp-factory-sources":
        raise SystemExit("fetch-samples: %s is not a pulp-factory-sources manifest" % path)
    source_ids = {s.get("id") for s in doc.get("sources", [])}
    entries = []
    seen = {}
    problems = []
    for inst in doc.get("instruments", []):
        for item in inst.get("files", []):
            rel = item.get("file", "")
            url = item.get("url", "")
            sha = (item.get("sha256") or "").lower()
            where = "%s / %s" % (inst.get("name", "?"), rel or "?")
            parts = rel.replace("\\", "/").split("/")
            if (not rel or rel.startswith(("/", "\\")) or ":" in rel
                    or any(p in ("", ".", "..") for p in parts)):
                problems.append("%s: unsafe or empty cache path" % where)
                continue
            if urllib.parse.urlsplit(url).scheme != "https":
                problems.append("%s: URL is not https" % where)
                continue
            if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
                problems.append("%s: manifest has no valid sha256" % where)
                continue
            if item.get("source") not in source_ids:
                problems.append("%s: unknown source id %r" % (where, item.get("source")))
                continue
            key = "/".join(parts).lower()
            if key in seen:
                problems.append("%s: cache path used twice" % where)
                continue
            seen[key] = True
            entries.append({"rel": "/".join(parts), "url": url, "sha256": sha,
                            "bytes": item.get("bytes"), "instrument": inst.get("name", "?")})
    if problems:
        for p in problems:
            say("fetch-samples: MANIFEST ERROR: " + p, err=True)
        raise SystemExit(1)
    return entries


def download_once(entry, dest):
    """Download to dest + '.part', verify, rename.  Returns bytes written."""
    part = dest + ".part"
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    req = urllib.request.Request(entry["url"], headers={"User-Agent": USER_AGENT})
    h = hashlib.sha256()
    size = 0
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp, open(part, "wb") as out:
            while True:
                block = resp.read(CHUNK)
                if not block:
                    break
                h.update(block)
                out.write(block)
                size += len(block)
        want_bytes = entry.get("bytes")
        if isinstance(want_bytes, int) and want_bytes != size:
            raise ValueError("size mismatch: got %d bytes, manifest says %d" % (size, want_bytes))
        got = h.hexdigest()
        if got != entry["sha256"]:
            raise ValueError("SHA-256 mismatch: got %s, manifest says %s" % (got, entry["sha256"]))
        os.replace(part, dest)
        return size
    except BaseException:
        try:
            os.remove(part)
        except OSError:
            pass
        raise


def process(entry, cache, check_only, verbose):
    """Returns (status, bytes, message); status is cached / downloaded / missing / bad / failed."""
    dest = os.path.join(cache, *entry["rel"].split("/"))
    stale = ""
    if os.path.isfile(dest):
        got, size = sha256_of(dest)
        if got == entry["sha256"]:
            return "cached", size, ""
        stale = "cached copy has SHA-256 %s, manifest says %s" % (got, entry["sha256"])
        if check_only:
            return "bad", size, stale
    elif check_only:
        return "missing", 0, "not in cache"

    last = ""
    for attempt in range(1 + RETRIES):
        try:
            size = download_once(entry, dest)
            if verbose:
                say("  got %s (%d bytes)" % (entry["rel"], size))
            return "downloaded", size, ""
        except (urllib.error.URLError, OSError, ValueError) as exc:
            last = "%s: %s" % (type(exc).__name__, exc)
            if attempt < RETRIES:
                say("  retry %d/%d for %s (%s)" % (attempt + 1, RETRIES, entry["rel"], last), err=True)
                time.sleep(1.5 * (attempt + 1))
    if stale:
        last += " (and the " + stale + ")"
    return "failed", 0, last


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    project = os.path.dirname(here)
    ap = argparse.ArgumentParser(description="Download the PULP factory sample sources into factory-cache/.")
    ap.add_argument("--check", action="store_true",
                    help="verify the cache against the manifest without downloading anything")
    ap.add_argument("--manifest", default=os.path.join(project, "factory", "sources.json"),
                    help="manifest to read (default: factory/sources.json)")
    ap.add_argument("--cache", default=os.path.join(project, "factory-cache"),
                    help="cache folder to fill (default: factory-cache/ beside factory/)")
    ap.add_argument("--jobs", type=int, default=6, help="parallel downloads (default 6)")
    ap.add_argument("--verbose", action="store_true", help="print every downloaded file")
    args = ap.parse_args(argv)

    entries = load_manifest(args.manifest)
    cache = os.path.abspath(args.cache)
    if not args.check:
        os.makedirs(cache, exist_ok=True)

    counts = {"cached": 0, "downloaded": 0, "missing": 0, "bad": 0, "failed": 0}
    sizes = {"cached": 0, "downloaded": 0}
    failures = []
    jobs = max(1, min(16, args.jobs))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(process, e, cache, args.check, args.verbose): e for e in entries}
        for fut in concurrent.futures.as_completed(futures):
            entry = futures[fut]
            try:
                status, size, message = fut.result()
            except Exception as exc:  # never lose a failure silently
                status, size, message = "failed", 0, "%s: %s" % (type(exc).__name__, exc)
            counts[status] += 1
            if status in sizes:
                sizes[status] += size
            if status in ("missing", "bad", "failed"):
                failures.append((entry["rel"], status, message))

    for rel, status, message in sorted(failures):
        say("fetch-samples: %s: %s: %s" % (status.upper(), rel, message), err=True)

    total_mb = (sizes["cached"] + sizes["downloaded"]) / 1e6
    if args.check:
        say("fetch-samples --check: %d files in manifest, %d verified (%.1f MB), %d missing, %d wrong checksum; cache = %s"
            % (len(entries), counts["cached"], total_mb, counts["missing"], counts["bad"], cache))
    else:
        say("fetch-samples: %d files in manifest, %d already cached, %d downloaded (%.1f MB), %d failed; cache = %s (%.1f MB verified)"
            % (len(entries), counts["cached"], counts["downloaded"], sizes["downloaded"] / 1e6,
               counts["failed"], cache, total_mb))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
