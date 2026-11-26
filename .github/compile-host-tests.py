#!/usr/bin/env python3
"""Compile only the targets needed by Meson's 'host' test suite, skipping Windows GPU test executables."""
import json
import os
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: compile-host-tests.py <build-dir>")
    build = os.path.abspath(sys.argv[1])
    tests = json.loads(subprocess.check_output(["meson", "introspect", "--tests", build], text=True))
    targets = {
        t["id"]: t for t in json.loads(subprocess.check_output(["meson", "introspect", "--targets", build], text=True))
    }
    wanted = []
    seen = set()
    for test in tests:
        if not any(s == "host" or s.endswith(":host") for s in test.get("suite", [])):
            continue
        for dep_id in test.get("depends", []):
            target = targets.get(dep_id)
            if not target:
                continue
            for path in target.get("filename", []):
                rel = os.path.relpath(path, build)
                if rel not in seen:
                    seen.add(rel)
                    wanted.append(rel)
    if not wanted:
        raise SystemExit("no host test targets found")
    print(f"Building {len(wanted)} host test targets (skipping GPU test executables)...")
    subprocess.run(["ninja", "-C", build] + wanted, check=True)


if __name__ == "__main__":
    main()
