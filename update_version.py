#!/usr/bin/env python3

# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

"""
Synchronise the STK version across the repository.

Reads the version from ``version.txt`` (repo root, a single line such as ``1.8.0``)
and updates:

  * stk/include/stk_version.h  -> STK_VERSION_MAJOR / STK_VERSION_MINOR / STK_VERSION_PATCH
  * library.json               -> "version"

The version must be plain Semantic Versioning ``MAJOR.MINOR.PATCH`` without leading
zeros (e.g. 1.8.0, 1.10.0). Each field must be in 0..255 because stk_version.h packs
the fields into one byte each.

Usage:
    python update_version.py            # update files in place
    python update_version.py --check    # verify only, exit 1 if files are out of sync (for CI)
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VERSION_FILE = ROOT / "version.txt"
HEADER_FILE = ROOT / "stk" / "include" / "stk_version.h"
LIBRARY_JSON = ROOT / "library.json"

# Plain SemVer core: no leading zeros, no pre-release/build suffix.
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
FIELD_MAX = 255  # one byte per field in STK_VERSION_ENCODE


def fail(message):
    print("error: " + message, file=sys.stderr)
    sys.exit(1)


def read_version():
    if not VERSION_FILE.is_file():
        fail("%s not found" % VERSION_FILE.name)

    lines = [ln.strip() for ln in VERSION_FILE.read_text(encoding="utf-8").splitlines()]
    lines = [ln for ln in lines if ln]
    if len(lines) != 1:
        fail("%s must contain exactly one line with the version, e.g. 1.8.0" % VERSION_FILE.name)

    text = lines[0]
    match = SEMVER_RE.match(text)
    if not match:
        fail("invalid version '%s' in %s: expected MAJOR.MINOR.PATCH without leading zeros "
             "(e.g. 1.8.0, 1.10.0)" % (text, VERSION_FILE.name))

    major, minor, patch = (int(g) for g in match.groups())
    for name, value in (("MAJOR", major), ("MINOR", minor), ("PATCH", patch)):
        if value > FIELD_MAX:
            fail("%s version field %d is out of range 0..%d" % (name, value, FIELD_MAX))

    return text, (major, minor, patch)


def read_text(path):
    # newline="" keeps the file's original line endings untouched.
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def update_header(text, parts):
    """Replace the three #define values, keeping the original spacing."""
    for name, value in zip(("MAJOR", "MINOR", "PATCH"), parts):
        pattern = re.compile(r"^(\s*#\s*define\s+STK_VERSION_%s\s+)\d+(\s*)$" % name, re.MULTILINE)
        text, count = pattern.subn(lambda m, v=value: "%s%d%s" % (m.group(1), v, m.group(2)), text)
        if count != 1:
            fail("could not find exactly one '#define STK_VERSION_%s <number>' in %s"
                 % (name, HEADER_FILE.relative_to(ROOT)))
    return text


def update_library_json(text, version):
    """Replace the top-level "version" value, keeping the file formatting as is."""
    try:
        data = json.loads(text)
    except ValueError as e:
        fail("%s is not valid JSON: %s" % (LIBRARY_JSON.name, e))
    if "version" not in data:
        fail('%s has no "version" field' % LIBRARY_JSON.name)

    pattern = re.compile(r'^(\s*"version"\s*:\s*")[^"]*(")', re.MULTILINE)
    text, count = pattern.subn(lambda m: m.group(1) + version + m.group(2), text, count=1)
    if count != 1:
        fail('could not locate the "version" line in %s' % LIBRARY_JSON.name)

    # Make sure the result is still valid JSON with the expected value.
    if json.loads(text)["version"] != version:
        fail("internal error: %s was not updated correctly" % LIBRARY_JSON.name)
    return text


def main():
    parser = argparse.ArgumentParser(description="Sync STK version from version.txt.")
    parser.add_argument("--check", action="store_true",
                        help="do not modify files; exit with 1 if they are out of sync")
    args = parser.parse_args()

    version, parts = read_version()

    for path in (HEADER_FILE, LIBRARY_JSON):
        if not path.is_file():
            fail("%s not found" % path.relative_to(ROOT))

    targets = (
        (HEADER_FILE, lambda t: update_header(t, parts)),
        (LIBRARY_JSON, lambda t: update_library_json(t, version)),
    )

    out_of_sync = False
    for path, updater in targets:
        old = read_text(path)
        new = updater(old)
        rel = path.relative_to(ROOT).as_posix()
        if new == old:
            print("ok       %s (already %s)" % (rel, version))
        elif args.check:
            print("MISMATCH %s (expected %s)" % (rel, version))
            out_of_sync = True
        else:
            write_text(path, new)
            print("updated  %s -> %s" % (rel, version))

    if args.check and out_of_sync:
        sys.exit(1)


if __name__ == "__main__":
    main()
