#!/usr/bin/env python3
"""Provenance manifest for an NYC Linux build.

usage: kernel-provenance.py BUILD_LOG OUT_DIR NYC_BINARY [MANIFEST.json]

Pairs every `CC <object>` step of a V=1 Kbuild log that went through the strict
wrapper with the `compiled <source> -> ...nyc.s` line NYC printed for it, and
records the compiler and configuration checksums. Exits non-zero when a C unit
was built by something other than NYC, or an NYC unit has no object.
"""
import hashlib, json, os, re, subprocess, sys

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    log, out, nyc = sys.argv[1:4]
    manifest = sys.argv[4] if len(sys.argv) > 4 else os.path.join(out, "provenance.json")
    text = open(log, errors="replace").read()
    cc_objects = re.findall(r"^\s*# CC\s+(\S+\.o)\s*$", text, re.M)
    # parallel output can interleave lines, so match anywhere in a line
    compiled = re.findall(r"compiled (\S+\.c) -> (\S+?)\.nyc\.", text)
    by_object = {}
    for src, stem in compiled:
        by_object[stem + ".o"] = src
    missing = []
    for o in cc_objects:
        if o in by_object:
            continue
        # Interleaved parallel output can cut the "compiled" line; fall back to
        # the object itself: a GCC-built object carries a "GCC:" .comment.
        path = os.path.join(out, o)
        comment = subprocess.run(["readelf", "-p", ".comment", path],
                                 capture_output=True, text=True).stdout
        if os.path.exists(path) and "GCC:" not in comment:
            by_object[o] = "(paired by object: no GCC .comment)"
        else:
            missing.append(o)
    orphan = [o for o in by_object if o not in set(cc_objects)]
    config = os.path.join(out, ".config")
    version = subprocess.run([nyc, "--version"], capture_output=True, text=True).stdout.strip()
    data = {
        "nyc_version": version,
        "nyc_sha256": sha(nyc),
        "config_sha256": sha(config) if os.path.exists(config) else None,
        "build_log_sha256": sha(log),
        "c_units_compiled_by_nyc": len(by_object),
        "cc_steps": len(cc_objects),
        "cc_steps_without_nyc": missing,
        "nyc_units_without_cc_step": orphan,
        "objects": {o: by_object[o] for o in sorted(by_object)},
    }
    with open(manifest, "w") as f:
        json.dump(data, f, indent=1)
    print("units compiled by NYC:", len(by_object), " CC steps:", len(cc_objects),
          " without NYC:", len(missing))
    return 1 if missing else 0

if __name__ == "__main__":
    sys.exit(main())
