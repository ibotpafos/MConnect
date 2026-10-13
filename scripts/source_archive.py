#!/usr/bin/env python3
"""Build the receiver's downloadable corresponding project source."""
from pathlib import Path
import sys
import zipfile

root = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
inputs = [p for folder in ("src", "cmake", "scripts", "assets", "docs", "tests")
          for p in (root / folder).rglob("*") if p.is_file() and "__pycache__" not in p.parts]
inputs += [root / name for name in ("CMakeLists.txt", "dependencies.lock.json", "LICENSE",
           "NOTICE.md", "README.md", "THIRD_PARTY_NOTICES.md", "LICENSES_THIRD_PARTY.md",
           ".gitignore", ".gitattributes", "CHANGELOG.md")
           if (root / name).is_file()]
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(set(inputs)):
        if path.resolve() != output and path.suffix not in (".pem", ".key", ".p12", ".pfx", ".pyc"):
            archive.write(path, "MConnect/" + path.relative_to(root).as_posix())
