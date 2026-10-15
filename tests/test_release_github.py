"""Regression: draft releases must not require a public tag endpoint."""
import importlib.util
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("mconnect_release", Path(__file__).resolve().parents[1] / "scripts/release_github.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseLookupTests(unittest.TestCase):
    def lookup(self, pages):
        calls = []

        def api(*args, **kwargs):
            calls.append(args)
            self.assertNotIn("/releases/tags/", args[-1])
            return SimpleNamespace(stdout=json.dumps(pages[len(calls)-1]), returncode=0)

        with patch.object(release, "run", side_effect=api):
            result = release.find_release("owner/project", "v0.1.4")
        return result, calls

    def test_draft_with_temporary_url(self):
        draft = {"id": 42, "tag_name": "v0.1.4", "draft": True,
                 "html_url": "https://github.com/owner/project/releases/tag/untagged-abc"}
        found, calls = self.lookup([[{"id": 1, "tag_name": "v0.1.3"}, draft]])
        self.assertEqual(found, draft)
        self.assertEqual(len(calls), 1)

    def test_pagination(self):
        draft = {"id": 42, "tag_name": "v0.1.4", "draft": True}
        found, calls = self.lookup([[{"id": n, "tag_name": f"v0.0.{n}"} for n in range(100)], [draft]])
        self.assertEqual(found["id"], 42)
        self.assertTrue(calls[1][-1].endswith("page=2"))

    def test_missing_release(self):
        self.assertIsNone(self.lookup([[]])[0])

    def test_duplicate_tag_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.lookup([[{"id": 1, "tag_name": "v0.1.4"}, {"id": 2, "tag_name": "v0.1.4"}]])

    def publish_fixture(self, corrupt=False):
        calls, published = [], []
        created = False
        with tempfile.TemporaryDirectory(prefix="mconnect-release-test-") as directory:
            package = Path(directory) / "MConnect-0.1.4.pkg"
            package.write_bytes(b"verified package fixture")
            asset = {"name": package.name, "size": package.stat().st_size,
                     "digest": "sha256:" + ("0" * 64 if corrupt else hashlib.sha256(package.read_bytes()).hexdigest())}
            draft = {"id": 42, "tag_name": "v0.1.4", "draft": True, "assets": [asset]}

            def command(*args, **kwargs):
                nonlocal created
                calls.append(args)
                self.assertFalse(any("/releases/tags/" in arg for arg in args))
                result = ""
                if args[:3] == ("git", "branch", "--show-current"):
                    result = "main\n"
                elif args[:3] == ("git", "remote", "get-url"):
                    result = "https://github.com/owner/project.git\n"
                elif args[:2] == ("git", "rev-parse"):
                    result = "abc123\n"
                elif args[:3] == ("gh", "release", "create"):
                    created = True
                elif args[:4] == ("gh", "api", "--method", "PATCH"):
                    self.assertEqual(args[4], "repos/owner/project/releases/42")
                    published.append(args)
                elif args[:2] == ("gh", "api"):
                    path = args[2]
                    if path == "repos/owner/project":
                        result = json.dumps({"private": False})
                    elif "/releases?" in path:
                        result = json.dumps([draft] if created else [])
                    elif path.endswith("/releases/42"):
                        result = json.dumps(draft)
                    else:
                        self.fail(f"Unexpected API operation: {path}")
                return SimpleNamespace(stdout=result, returncode=0)

            with patch.object(release, "run", side_effect=command):
                if corrupt:
                    with self.assertRaises(SystemExit):
                        release.publish("owner/project", "v0.1.4", [package], "Fixture notes", False, True)
                else:
                    release.publish("owner/project", "v0.1.4", [package], "Fixture notes", False, True)
        return published

    def test_new_draft_is_verified_and_published_by_id(self):
        published = self.publish_fixture()
        self.assertEqual(len(published), 1)
        self.assertIn("draft=false", published[0])

    def test_changed_asset_cannot_be_published(self):
        self.assertEqual(self.publish_fixture(corrupt=True), [])


if __name__ == "__main__":
    unittest.main()
