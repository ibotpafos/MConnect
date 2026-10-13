#!/usr/bin/env python3
"""Fetch exact source revisions locally. Never installs global packages."""
import concurrent.futures
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent

def git(*args, cwd=None):
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()

def fetch(item):
    name, spec = item
    destination = ROOT / "external" / name
    if not (destination / ".git").exists():
        git("clone", "--quiet", "--depth", "1", "--branch", spec["tag"], spec["url"], str(destination))
    current = git("rev-parse", "HEAD", cwd=destination)
    if current != spec["commit"]:
        raise RuntimeError(f"{name}: expected {spec['commit']}, found {current}. Refusing to modify existing checkout.")
    if spec.get("recursive"):
        git("submodule", "update", "--init", "--recursive", "--depth", "1", cwd=destination)
    print(f"{name}: verified {current[:12]}", flush=True)

if __name__ == "__main__":
    (ROOT / "external").mkdir(exist_ok=True)
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(fetch, lock.items()))
