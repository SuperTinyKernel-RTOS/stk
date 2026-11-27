#!/usr/bin/env python3
#
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
#
#  Source: https://github.com/SuperTinyKernel-RTOS
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#
"""Generate an SPDX 2.3 (JSON) SBOM for ONE build configuration of the STK library.

Usage:
  gen_sbom.py --output <stk.spdx.json> [--root <repo-root>] [--components <components.json>]
              [--define NAME=VALUE ...]

The --define values are the CMake variables that describe the configuration (VENDOR_STM32, TARGET_CORTEX_M4,
ENABLE_SEGGER_SYSVIEW, ...). They are matched against the "when" conditions of build/sbom/components.json, so the SBOM lists
exactly the third-party code which this configuration brings in. Values follow the CMake truthiness rules.

Output is deterministic: the creation time is SOURCE_DATE_EPOCH, else the date of the git commit, else the mtime of
version.txt. The file is rewritten only if its content changed.

Exit status: 0 on success or when the configuration is not covered yet (prints "SKIPPED: <reason>" and writes nothing),
1 on errors in the metadata (unknown license reference, missing path, ...): a wrong SBOM is worse than no SBOM.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

TOOL_NAME = "stk-gen-sbom"
TOOL_VERSION = "1.0"
NAMESPACE_BASE = "https://github.com/SuperTinyKernel-RTOS/stk/spdx"
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
LICENSE_REF_RE = re.compile(r"LicenseRef-[A-Za-z0-9.\-]+")
NOASSERTION = "NOASSERTION"

# CMake "false constants" (everything else, if defined, is true)
_CMAKE_FALSE = {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}


class SbomError(Exception):
    pass


def is_true(value):
    """CMake semantics of if(<variable>)."""
    if value is None:
        return False
    text = str(value).strip().upper()
    return not (text in _CMAKE_FALSE or text.endswith("-NOTFOUND"))


def git(root, *args):
    """Run git in root, return stripped stdout or None on any failure."""
    try:
        result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=True,
                                timeout=30)
        return result.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def repo_state(root):
    """Return {'hash', 'date', 'dirty'} if root IS a git repository top level, otherwise None (tarball, vendored copy)."""
    toplevel = git(root, "rev-parse", "--show-toplevel")
    if toplevel is None or Path(toplevel).resolve() != root.resolve():
        return None
    full_hash = git(root, "rev-parse", "HEAD")
    if not full_hash:
        return None
    return {
        "hash": full_hash,
        "date": git(root, "log", "-1", "--format=%cI"),
        "dirty": bool(git(root, "status", "--porcelain", "--untracked-files=no")),
    }


def read_version(root):
    text = (root / "version.txt").read_text(encoding="utf-8").strip()
    if not SEMVER_RE.match(text):
        raise SbomError("version.txt: '%s' is not MAJOR.MINOR.PATCH" % text)
    return text


def creation_time(root, state):
    """Reproducible UTC timestamp, see the module docstring."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch and epoch.isdigit():
        moment = datetime.fromtimestamp(int(epoch), tz=timezone.utc)
    elif state and state["date"]:
        moment = datetime.fromisoformat(state["date"]).astimezone(timezone.utc)
    else:
        moment = datetime.fromtimestamp((root / "version.txt").stat().st_mtime, tz=timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def condition_holds(cond, defs):
    """cond: {"all": [...], "any": [...], "none": [...]} over CMake variable names."""
    cond = cond or {}
    if not all(is_true(defs.get(n)) for n in cond.get("all", [])):
        return False
    if cond.get("any") and not any(is_true(defs.get(n)) for n in cond["any"]):
        return False
    return not any(is_true(defs.get(n)) for n in cond.get("none", []))


def platform_name(defs):
    if is_true(defs.get("VENDOR_STM32")):
        for flag, name in (("TARGET_CORTEX_M0", "stm32f0"), ("TARGET_CORTEX_M3", "stm32f1"),
                           ("TARGET_CORTEX_M4", "stm32f4"), ("TARGET_CORTEX_M7", "stm32f7")):
            if is_true(defs.get(flag)):
                return name
        return "stm32"
    if is_true(defs.get("RISCV")):
        return "riscv"
    if is_true(defs.get("ARM")):
        return "arm-cortex-m"
    return "generic"


def spdx_id(text):
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.\-]", "-", text)


def package(comp, sid, version=None):
    """Common SPDX package fields of a component (root or third-party)."""
    pkg = {
        "SPDXID": sid,
        "name": comp["name"],
        "downloadLocation": comp.get("download", NOASSERTION),
        "filesAnalyzed": False,
        "licenseConcluded": comp["license"],
        "licenseDeclared": comp["license"],
        "copyrightText": comp.get("copyright", NOASSERTION),
        "supplier": comp.get("supplier", NOASSERTION),
    }
    if version:
        pkg["versionInfo"] = version
    if comp.get("description"):
        pkg["description"] = comp["description"]
    if comp.get("homepage"):
        pkg["homepage"] = comp["homepage"]
    if comp.get("purl"):
        locator = comp["purl"] + ("@" + version if version and "@" not in comp["purl"] else "")
        pkg["externalRefs"] = [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                                "referenceLocator": locator}]
    return pkg


def build_document(root, meta, defs):
    version = read_version(root)
    state = repo_state(root)
    platform = platform_name(defs)

    # --- configuration identity -----------------------------------------------------------------------------------
    config = sorted((k, str(v)) for k, v in defs.items() if is_true(v))
    config_text = ", ".join("%s=%s" % kv for kv in config)
    config_hash = hashlib.sha256(config_text.encode("utf-8")).hexdigest()[:8]
    short = state["hash"][:8] if state else "nogit"
    revision = (state["hash"] + (" (working tree has local modifications)" if state["dirty"] else "")) if state \
        else "unknown (not built from a git checkout)"

    # --- root package -----------------------------------------------------------------------------------------------
    root_meta = meta["root"]
    root_id = "SPDXRef-" + root_meta["name"]
    root_pkg = package(root_meta, root_id, version)
    root_pkg["primaryPackagePurpose"] = "LIBRARY"
    root_pkg["sourceInfo"] = "git revision: " + revision
    root_pkg["comment"] = "Build configuration: " + (config_text or "default")
    if state:
        root_pkg["downloadLocation"] = "%s@%s" % (root_meta["download"], state["hash"])
    packages = [root_pkg]
    relationships = [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                      "relatedSpdxElement": root_id}]

    # --- third-party components selected by this configuration -----------------------------------------------------
    used_refs = set(LICENSE_REF_RE.findall(root_meta["license"]))
    for comp in meta["components"]:
        if not condition_holds(comp.get("when"), defs):
            continue
        for rel_path in comp.get("paths", []):
            if not (root / rel_path).exists():
                raise SbomError("component '%s': path '%s' does not exist (update build/sbom/components.json)"
                                % (comp["id"], rel_path))
        sid = spdx_id(comp["id"])
        pkg = package(comp, sid, comp.get("version"))
        notes = []
        if comp.get("comment"):
            notes.append(comp["comment"])
        if comp.get("paths"):
            notes.append("Located in: " + ", ".join(comp["paths"]))
        pkg["comment"] = " ".join(notes)
        packages.append(pkg)
        relationship = {"spdxElementId": root_id, "relationshipType": comp.get("relationship", "DEPENDS_ON"),
                        "relatedSpdxElement": sid}
        relationships.append(relationship)
        used_refs.update(LICENSE_REF_RE.findall(comp["license"]))

    # --- custom licenses ---------------------------------------------------------------------------------------------
    extracted = []
    for ref in sorted(used_refs):
        entry = meta.get("license_refs", {}).get(ref)
        if entry is None:
            raise SbomError("license '%s' is used but not defined in license_refs" % ref)
        text_path = root / entry["file"]
        if not text_path.is_file():
            raise SbomError("license text '%s' of '%s' not found" % (entry["file"], ref))
        extracted.append({"licenseId": ref, "name": entry["name"],
                          "extractedText": text_path.read_text(encoding="utf-8").strip()})

    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "%s-%s-%s" % (root_meta["name"], version, platform),
        "documentNamespace": "%s/%s-%s-%s-%s-%s" % (NAMESPACE_BASE, root_meta["name"], version, platform, short,
                                                    config_hash),
        "creationInfo": {
            "created": creation_time(root, state),
            "creators": ["Tool: %s-%s" % (TOOL_NAME, TOOL_VERSION), "Organization: Neutron Code Limited"],
            "comment": meta.get("scope_note", ""),
        },
        "packages": packages,
        "relationships": relationships,
    }
    if extracted:
        document["hasExtractedLicensingInfos"] = extracted
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", required=True, type=Path, help="SPDX JSON file to write")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="STK repository root (default: two levels above this script)")
    parser.add_argument("--components", type=Path, help="components.json (default: <root>/build/sbom/components.json)")
    parser.add_argument("--define", action="append", default=[], metavar="NAME=VALUE",
                        help="CMake variable describing the configuration (repeatable)")
    args = parser.parse_args()

    root = args.root.resolve()
    defs = {}
    for item in args.define:
        name, _, value = item.partition("=")
        defs[name.strip()] = value

    try:
        meta = json.loads((args.components or root / "build" / "sbom" / "components.json").read_text(encoding="utf-8"))
        unsupported = meta.get("unsupported_when")
        if unsupported and condition_holds(unsupported, defs):
            print("SKIPPED: " + unsupported.get("reason", "configuration is not covered"))
            return 0
        content = json.dumps(build_document(root, meta, defs), indent=2, ensure_ascii=False) + "\n"
    except (SbomError, OSError, ValueError, KeyError) as err:
        print("gen_sbom: error: %s" % err, file=sys.stderr)
        return 1

    try:
        if args.output.read_text(encoding="utf-8") == content:
            return 0
    except OSError:
        pass
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8")
    print("gen_sbom: wrote %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
