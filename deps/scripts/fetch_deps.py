#!/usr/bin/env python3
#
#  STK Dependency Fetcher: Utility script to fetch/clone all STK dependencies.
#
#  Dependencies (URL, destination, branch, revision) are listed in deps.py,
#  located next to this script.
#
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#
"""Fetch or update git dependencies listed in deps.py.

Usage: fetch_deps.py [--list] [--keep-going] [--revision NAME=REV ...]
                     [--branch NAME=BRANCH ...] [name ...]

Clones each repository if it does not exist, or updates it if already present.
Falls back from 'main' to 'master' (or vice versa) if the branch is not found.
A pinned 'revision' (commit hash, tag or branch) takes precedence over 'branch'.
Initializes/updates submodules automatically if .gitmodules is present.
With no names given, all dependencies are fetched.

One-off overrides (not written back to deps.py):
    --revision NAME=REV      pin NAME to REV; an empty REV ("NAME=") unpins it
    --branch NAME=BRANCH     track BRANCH for NAME (ignored while a revision is pinned)
Both options may be repeated.
"""

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True # prevent __pycache___
FETCH_ROOT = Path(__file__).resolve().parent  # relative to the script, not the caller's cwd
GIT_TIMEOUT = 300  # seconds per git command


def log_err(msg: str) -> None:
    print(f"fetch_deps: {msg}", file=sys.stderr)


def load_deps() -> list[dict]:
    """Load the DEPS list from deps.py next to this script."""
    path = FETCH_ROOT / "deps.py"
    spec = importlib.util.spec_from_file_location("deps", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    deps = list(module.DEPS)
    names = set()
    for d in deps:
        for key in ("name", "url", "dest"):
            if not d.get(key):
                raise ValueError(f"{path}: dependency entry missing '{key}': {d}")
        if d["name"] in names:
            raise ValueError(f"{path}: duplicate dependency name '{d['name']}'")
        names.add(d["name"])
    return deps


def parse_overrides(items: list[str], flag: str, known: set[str]) -> dict[str, str]:
    """Parse repeated NAME=VALUE command-line items into a dict, validating names."""
    result: dict[str, str] = {}
    for item in items:
        name, sep, value = item.partition("=")
        if not sep or not name:
            raise ValueError(f"{flag} expects NAME=VALUE, got '{item}'")
        if name not in known:
            raise ValueError(f"{flag}: unknown dependency '{name}' "
                             f"(available: {', '.join(sorted(known))})")
        result[name] = value
    return result


def run_cmd(cmd: list[str], cwd: Path | None = None) -> bool:
    """Run a command, return True on success, False on failure."""
    try:
        subprocess.run(cmd, cwd=cwd, check=True, timeout=GIT_TIMEOUT)
        return True
    except (OSError, subprocess.SubprocessError) as e:
        log_err(f"error executing {' '.join(cmd)}: {e}")
        return False


def other_branch(branch: str) -> str | None:
    """Return the main/master counterpart of a branch name, if any."""
    return {"main": "master", "master": "main"}.get(branch)


def update_submodules(dest: Path) -> bool:
    """Initialize/update submodules if .gitmodules is present."""
    if (dest / ".gitmodules").exists():
        print("Found .gitmodules; initializing and updating submodules...")
        return run_cmd(["git", "submodule", "update", "--init", "--recursive"], cwd=dest)
    return True


def clone_repo(url: str, dest: Path, branch: str, revision: str | None) -> bool:
    """Clone the repository, checking out the given revision or branch."""
    if revision:
        print(f"Cloning repository (revision/branch: {revision})...")
        if run_cmd(["git", "clone", "--branch", revision, url, str(dest)]):
            return update_submodules(dest)
        log_err(f"direct clone with '{revision}' failed (may be a commit hash), "
                "attempting standard clone...")
        if not run_cmd(["git", "clone", url, str(dest)]):
            return False
        if not run_cmd(["git", "checkout", revision], cwd=dest):
            return False
        return update_submodules(dest)

    candidates = [branch]
    if (alt := other_branch(branch)):
        candidates.append(alt)
    for b in candidates:
        print(f"Cloning repository (branch: {b})...")
        if run_cmd(["git", "clone", "--branch", b, url, str(dest)]):
            return update_submodules(dest)
        log_err(f"branch '{b}' not found or clone failed, trying alternatives...")
    return False


def update_repo(dest: Path, branch: str, revision: str | None) -> bool:
    """Update an existing clone to the given revision or latest branch state."""
    print("Repository already exists. Updating latest changes...")
    if not run_cmd(["git", "fetch", "origin"], cwd=dest):
        return False

    target = revision or branch
    print(f"Checking out target: {target}")
    if not run_cmd(["git", "checkout", target], cwd=dest):
        fallback = other_branch(branch) if not revision else None
        if not fallback:
            return False
        print(f"Branch '{branch}' checkout failed, attempting fallback to '{fallback}'...")
        if not run_cmd(["git", "checkout", fallback], cwd=dest):
            return False
        branch = fallback

    if revision:
        # Pinned revision (possibly a bare commit hash): keep the exact state.
        print("Revision specified; skipping git pull to maintain exact target state.")
    elif not run_cmd(["git", "pull", "origin", branch], cwd=dest):
        return False

    return update_submodules(dest)


def fetch_dep(dep: dict) -> bool:
    """Fetch or update a single dependency described by a deps.py entry."""
    name = dep["name"]
    url = dep["url"]
    dest = (FETCH_ROOT / dep["dest"]).resolve()
    branch = dep.get("branch") or "main"
    revision = dep.get("revision") or None

    pin = f" @ {revision}" if revision else f" (branch {branch})"
    print(f"[{name}] Fetching {url}{pin} -> {dest}")

    if (dest / ".git").exists():
        ok = update_repo(dest, branch, revision)
    else:
        dest.parent.mkdir(parents=True, exist_ok=True)
        ok = clone_repo(url, dest, branch, revision)

    if ok:
        print(f"[{name}] Successfully fetched/updated.")
    else:
        log_err(f"[{name}] error: all clone/update attempts failed.")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("names", nargs="*", metavar="name",
                        help="dependencies to fetch (default: all)")
    parser.add_argument("--revision", action="append", default=[], metavar="NAME=REV",
                        help="override the revision (commit/tag/branch) of a dependency; "
                             "empty REV unpins it; repeatable")
    parser.add_argument("--branch", action="append", default=[], metavar="NAME=BRANCH",
                        help="override the tracked branch of a dependency "
                             "(ignored while a revision is pinned); repeatable")
    parser.add_argument("--list", action="store_true",
                        help="list configured dependencies and exit")
    parser.add_argument("--keep-going", action="store_true",
                        help="continue with remaining dependencies after a failure")
    args = parser.parse_args()

    try:
        deps = load_deps()
    except (OSError, ImportError, AttributeError, ValueError, SyntaxError) as e:
        log_err(f"cannot read deps.py: {e}")
        return 1

    known = {d["name"] for d in deps}
    try:
        revisions = parse_overrides(args.revision, "--revision", known)
        branches = parse_overrides(args.branch, "--branch", known)
    except ValueError as e:
        log_err(str(e))
        return 2

    # Apply overrides to copies, leaving the deps.py data untouched.
    effective = []
    for d in deps:
        d = dict(d)
        if d["name"] in branches:
            d["branch"] = branches[d["name"]] or d.get("branch")
        if d["name"] in revisions:
            d["revision"] = revisions[d["name"]] or None
        if d["name"] in branches and d.get("revision"):
            log_err(f"[{d['name']}] warning: --branch ignored, revision "
                    f"'{d['revision']}' is pinned (use --revision {d['name']}= to unpin)")
        effective.append(d)
    deps = effective

    if args.list:
        for d in deps:
            print(f"{d['name']:<20} {d['url']}  rev={d.get('revision') or '-'}  "
                  f"branch={d.get('branch') or 'main'}  dest={d['dest']}")
        return 0

    if args.names:
        unknown = [n for n in args.names if n not in known]
        if unknown:
            log_err(f"unknown dependencies: {', '.join(unknown)} "
                    f"(available: {', '.join(sorted(known))})")
            return 2
        deps = [d for d in deps if d["name"] in args.names]

    failed = []
    for dep in deps:
        if not fetch_dep(dep):
            failed.append(dep["name"])
            if not args.keep_going:
                break

    if failed:
        log_err(f"failed: {', '.join(failed)}")
        return 1

    print("All dependencies fetched/updated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
