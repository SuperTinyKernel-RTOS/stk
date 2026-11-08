#!/usr/bin/env python3
#
#  Infineon Core Library Fetcher: Utility script to fetch/clone core-lib.
#
#  Source: https://github.com/Infineon/core-lib
#
#  Copyright (c) Infineon Technologies AG. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
#
"""Fetch or update https://github.com/Infineon/core-lib.git in the target directory.

Usage: fetch_core_lib.py [--dest <path>] [--branch <branch>]

Clones the repository if it does not exist, or updates it if it is already present.
Automatically falls back from 'main' to 'master' (or vice versa) if the branch is not found.
"""

import argparse
import subprocess
import sys
from pathlib import Path


def run_cmd(cmd: list[str], cwd: Path | None = None) -> bool:
    """Run a command, return True on success, False on failure."""
    try:
        subprocess.run(
            cmd,
            cwd=cwd,
            check=True,
            timeout=120,
        )
        return True
    except (OSError, subprocess.SubprocessError) as e:
        print(f"fetch_core_lib: error executing {' '.join(cmd)}: {e}", file=sys.stderr)
        return False


def clone_repo(repo_url: str, dest_path: Path, branch: str) -> bool:
    """Attempt to clone the repository with a given branch, falling back if needed."""
    # Determine fallback branches to try if the primary attempt fails
    branches_to_try = [branch]
    if branch == "main":
        branches_to_try.append("master")
    elif branch == "master":
        branches_to_try.append("main")

    for b in branches_to_try:
        print(f"Cloning repository (branch: {b})...")
        cmd = ["git", "clone", "--branch", b, repo_url, str(dest_path)]
        if run_cmd(cmd):
            return True
        print(f"Branch '{b}' not found or clone failed, trying alternatives...", file=sys.stderr)

    return False


def main() -> int:
    fetch_root = Path(__file__).resolve().parent # be relative to the script's location, not caller's Path.cwd()

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dest", type=Path, default=fetch_root / "infineon" / "core-lib",
                        help="destination directory (default: ./infineon/core-lib)")
    parser.add_argument("--branch", type=str, default="main",
                        help="branch, tag, or commit to checkout (default: main)")
    args = parser.parse_args()

    repo_url = "https://github.com/Infineon/core-lib.git"
    dest_path = args.dest.resolve()

    print(f"Fetching {repo_url} -> {dest_path}")

    if (dest_path / ".git").exists():
        print("Repository already exists. Updating latest changes...")
        if not run_cmd(["git", "fetch", "origin"], cwd=dest_path):
            return 1
        
        # Try checking out requested branch, fallback to master/main if checkout fails
        if not run_cmd(["git", "checkout", args.branch], cwd=dest_path):
            fallback = "master" if args.branch == "main" else "main"
            print(f"Branch '{args.branch}' checkout failed, attempting fallback to '{fallback}'...")
            if not run_cmd(["git", "checkout", fallback], cwd=dest_path):
                return 1
            args.branch = fallback

        if not run_cmd(["git", "pull", "origin", args.branch], cwd=dest_path):
            return 1
    else:
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        if not clone_repo(repo_url, dest_path, args.branch):
            print("fetch_core_lib: error: all branch attempts failed.", file=sys.stderr)
            return 1

    print("Successfully fetched/updated core-lib.")
    return 0


if __name__ == "__main__":
    sys.exit(main())