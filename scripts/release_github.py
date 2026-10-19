#!/usr/bin/env python3
"""Validate a release locally; upload/publish only with an explicit flag."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(*args, capture=False, check=True):
    return subprocess.run(args, cwd=ROOT, check=check, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)


def prepare():
    cmake = (ROOT / "CMakeLists.txt").read_text()
    version = re.search(r"project\(MConnect VERSION ([\d.]+)", cmake).group(1)
    repository = re.search(r'set\(MCONNECT_RELEASE_REPOSITORY "([\w./-]+)"', cmake).group(1)
    base = f"MConnect-{version}-macos-universal"
    files = [ROOT / "dist" / f"{base}{extension}" for extension in (".pkg", ".zip", ".sha256")]
    files.append(ROOT / "dist" / f"MConnect-{version}-source.zip")
    if not all(path.is_file() for path in files):
        raise SystemExit("Build and package the current version first.")
    expected = dict((name, digest) for digest, name in
                    (line.split() for line in files[2].read_text().splitlines() if line.strip()))
    for path in (files[0], files[1], files[3]):
        if expected.get(path.name) != hashlib.sha256(path.read_bytes()).hexdigest():
            raise SystemExit(f"Checksum mismatch: {path.name}")
    run("python3", "tests/installer.py", str(files[0]))
    # Fresh sources, versions and executable payloads are checked by installer.py.
    tag = f"v{version}"
    changes = (ROOT / "CHANGELOG.md").read_text()
    match = re.search(rf"^## {re.escape(version)}[^\n]*\n(.*?)(?=^## |\Z)",
                      changes, re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit("Missing changelog entry for this release.")
    notes = (f"macOS 11+, Apple Silicon and Intel. AU + VST3.\n\n"
             f"{match.group(1).strip()}\n\n"
             "Install the .pkg for your macOS account with your DAW closed. "
             "Plugin signatures are verified; this build is not notarized by Apple.\n\n"
             "AGPL-3.0-only. Corresponding source and third-party notices are included.\n")
    print(f"Repository: https://github.com/{repository}\nTag: {tag}\nAssets:")
    for path in files:
        print(f"  {path.name} ({path.stat().st_size:,} bytes)")
    print(f"\nRelease notes:\n{notes}")
    return repository, tag, files, notes


def find_release(repository, tag):
    # Drafts can have an untagged-* URL and return 404 from /releases/tags/.
    # The authenticated listing includes them; use the returned numeric ID.
    page = 1
    while True:
        releases = json.loads(run("gh", "api",
            f"repos/{repository}/releases?per_page=100&page={page}", capture=True).stdout)
        matches = [release for release in releases if release.get("tag_name") == tag]
        if len(matches) > 1:
            raise SystemExit("Multiple releases use this tag; resolve the drafts before publishing.")
        if matches:
            return matches[0]
        if len(releases) < 100:
            return None
        page += 1


def publish(repository, tag, files, notes, create_repository, public):
    if run("git", "status", "--porcelain", capture=True).stdout.strip():
        raise SystemExit("Commit the reviewed sources before upload.")
    if run("git", "branch", "--show-current", capture=True).stdout.strip() != "main":
        raise SystemExit("Upload from the reviewed main branch.")
    configured = run("git", "remote", "get-url", "origin", capture=True, check=False)
    allowed = (f"https://github.com/{repository}", f"https://github.com/{repository}.git",
               f"git@github.com:{repository}.git")
    if configured.returncode == 0 and configured.stdout.strip() not in allowed:
        raise SystemExit("Existing origin does not match the release repository.")
    existing = run("gh", "api", f"repos/{repository}", capture=True, check=False)
    if existing.returncode:
        if not create_repository or "404" not in existing.stderr:
            raise SystemExit("Repository is unavailable. Creation requires --create-repository.")
        run("gh", "repo", "create", repository, "--public", "--description",
            "Lightweight AU/VST3 plugin: listen to your DAW on a phone browser over local Wi-Fi.")
    elif json.loads(existing.stdout).get("private"):
        raise SystemExit("GD Connect requires public releases; this repository is private.")
    if configured.returncode:
        run("git", "remote", "add", "origin", f"https://github.com/{repository}.git")
    current = run("git", "rev-parse", "HEAD", capture=True).stdout.strip()
    old_tag = run("git", "rev-parse", f"{tag}^{{commit}}", capture=True, check=False)
    if old_tag.returncode:
        run("git", "tag", "-a", tag, "-m", f"MConnect {tag[1:]}")
    elif old_tag.stdout.strip() != current:
        raise SystemExit("Release tag already points to different source; never move a published tag.")
    run("git", "push", "--atomic", "origin", "HEAD:refs/heads/main", f"refs/tags/{tag}")
    release = find_release(repository, tag)
    if release is None:
        with tempfile.NamedTemporaryFile("w", suffix=".md") as note_file:
            note_file.write(notes)
            note_file.flush()
            run("gh", "release", "create", tag, *(str(path) for path in files), "--repo", repository,
                "--verify-tag", "--draft", "--title", f"MConnect {tag[1:]}", "--notes-file", note_file.name)
        release = find_release(repository, tag)
        if release is None:
            raise SystemExit("Draft is not visible yet. Retry after GitHub finishes creating it.")
    elif not release.get("draft"):
        raise SystemExit("This release is already published; assets must remain immutable.")
    release_id = release["id"]
    verified = json.loads(run("gh", "api", f"repos/{repository}/releases/{release_id}", capture=True).stdout)
    if verified.get("tag_name") != tag or not verified.get("draft"):
        raise SystemExit("Draft identity or publication state changed; nothing was published.")
    assets = verified["assets"]
    by_name = {asset["name"]: asset for asset in assets}
    for path in files:
        digest = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
        asset = by_name.get(path.name, {})
        if asset.get("digest") != digest or asset.get("size") != path.stat().st_size:
            raise SystemExit(f"GitHub asset is missing or differs: {path.name}. Draft kept unpublished.")
    if public:
        run("gh", "api", "--method", "PATCH", f"repos/{repository}/releases/{release_id}",
            "--field", "draft=false", "--raw-field", "make_latest=true")
        print(f"Published: https://github.com/{repository}/releases/tag/{tag}")
    else:
        print("Verified draft uploaded; publish it when ready.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--upload-draft", action="store_true", help="Push reviewed main and upload a verified draft")
    mode.add_argument("--publish", action="store_true", help="Push main and publish the verified release")
    parser.add_argument("--create-repository", action="store_true", help="Allow creating the named public repository")
    args = parser.parse_args()
    if args.create_repository and not (args.upload_draft or args.publish):
        parser.error("Repository creation needs an explicit upload/publish mode.")
    prepared = prepare()
    if args.upload_draft or args.publish:
        publish(*prepared, args.create_repository, args.publish)
    else:
        print("Preview only. No GitHub write, remote, push or tag was performed.")


if __name__ == "__main__":
    main()
