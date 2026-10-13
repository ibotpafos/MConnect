#!/usr/bin/env python3
"""Verify the native macOS installer without starting any audio host."""
import argparse
from pathlib import Path
import plistlib
import subprocess
import tempfile
import uuid
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parent.parent
BUNDLES = ("Library/Audio/Plug-Ins/Components/MConnect.component",
           "Library/Audio/Plug-Ins/VST3/MConnect.vst3")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def output(*arguments):
    return subprocess.check_output([str(argument) for argument in arguments], text=True)


def verify(package, install=False):
    with tempfile.TemporaryDirectory(prefix="mconnect-installer-check-") as directory:
        expanded = Path(directory) / "expanded"
        output("pkgutil", "--expand-full", package, expanded)
        distribution = ET.parse(expanded / "Distribution").getroot()
        domains = distribution.find("domains")
        require(domains is not None and domains.attrib == {
            "enable_anywhere": "false", "enable_localSystem": "false",
            "enable_currentUserHome": "true"}, "Installer must target only the current user's home")
        options = distribution.find("options")
        require(options is not None and options.get("customize") == "never"
                and options.get("require-scripts") == "false"
                and set(options.get("hostArchitectures", "").split(",")) == {"arm64", "x86_64"},
                "Unexpected installer options or architectures")
        require(distribution.find("volume-check/allowed-os-versions/os-version").get("min") == "11.0",
                "Incorrect minimum macOS version")
        require(not list(expanded.rglob("Scripts")), "Installer must not run package scripts")
        packages = list(expanded.glob("*.pkg"))
        require({path.name for path in packages} == {"MConnect-au.pkg", "MConnect-vst3.pkg", "MConnect-docs.pkg"},
                "AU, VST3 and documentation must have separate component packages")
        payloads = {}
        package_versions = set()
        for component in packages:
            kind = component.stem.removeprefix("MConnect-")
            info = ET.parse(component / "PackageInfo").getroot()
            require(info.get("identifier") == f"audio.mconnect.pkg.{kind}"
                    and info.get("install-location") == "/", "Unexpected package identifier or base path")
            package_versions.add(info.get("version"))
            if kind != "docs":
                require(info.get("relocatable") == "false" and not info.findall("relocate/*"),
                        "Bundle relocation must be disabled")
                for setting in ("bundle-version", "upgrade-bundle", "strict-identifier"):
                    require(len(info.findall(f"{setting}/bundle")) == 1, f"Missing bundle behavior: {setting}")
            payload = component / "Payload"
            payloads[kind] = payload
            require(set(path.name for path in payload.iterdir()) == {"Library"},
                    "Payload contains files outside Library")
            for path in payload.rglob("*"):
                require(path.suffix not in (".app", ".dylib", ".pem", ".key", ".p12", ".pfx")
                        and path.name != "__pycache__", f"Unexpected shipped file: {path}")
        versions = set()
        for kind, relative in zip(("au", "vst3"), BUNDLES):
            payload = payloads[kind]
            bundle = payload / relative
            with (bundle / "Contents/Info.plist").open("rb") as stream:
                bundle_info = plistlib.load(stream)
            versions.add(bundle_info["CFBundleShortVersionString"])
            binary = bundle / "Contents/MacOS" / bundle_info["CFBundleExecutable"]
            require(binary.stat().st_mode & 0o111, f"Missing executable permissions: {binary}")
            require(set(output("lipo", "-archs", binary).split()) == {"arm64", "x86_64"},
                    f"Missing architecture: {binary}")
            output("codesign", "--verify", "--deep", "--strict", bundle)
        require(len(versions) == 1 and versions == package_versions, "Package and plugin versions differ")
        version = versions.pop()
        require(distribution.find("product").get("version") == version,
                "Product and plugin versions differ")
        documentation = payloads["docs"] / "Library/Application Support/MConnect"
        for relative in ("LICENSE", "NOTICE.md", "THIRD_PARTY_NOTICES.md", "LICENSES_THIRD_PARTY.md",
                         "README.md", "CHANGELOG.md", "docs/INSTALL.md"):
            require((documentation / relative).read_bytes() == (ROOT / relative).read_bytes(),
                    f"Missing or stale shipped document: {relative}")
        with zipfile.ZipFile(documentation / "MConnect-source.zip") as source:
            require(source.testzip() is None, "Source ZIP is corrupt")
            for name in source.namelist():
                relative = Path(name).relative_to("MConnect")
                require(".." not in relative.parts and not relative.is_absolute(), "Unsafe source path")
                require(source.read(name) == (ROOT / relative).read_bytes(), f"Stale source: {relative}")
            require("MConnect/scripts/package_macos.py" in source.namelist(),
                    "Missing installer build source")
        source_bytes = (documentation / "MConnect-source.zip").read_bytes()
        for kind, relative in zip(("au", "vst3"), BUNDLES):
            binary = payloads[kind] / relative / "Contents/MacOS/MConnect"
            require(source_bytes in binary.read_bytes(), f"Embedded source archive differs: {binary}")
        resources = expanded / "Resources"
        require((resources / "license.txt").read_bytes() == (ROOT / "LICENSE").read_bytes(),
                "Installer license is missing")
        for language in ("en.lproj", "ru.lproj"):
            for name in ("welcome.html", "conclusion.html"):
                require((resources / language / name).is_file(), f"Missing localized screen: {language}/{name}")
        domains_output = output("/usr/sbin/installer", "-dominfo", "-pkg", package)
        require("CurrentUserHomeDirectory" in domains_output, "Home installation is unavailable")
        require("LocalSystem" not in domains_output, "System installation must be unavailable")
        output("/usr/sbin/installer", "-showChoicesXML", "-pkg", package,
               "-target", "CurrentUserHomeDirectory")
        if install:
            probes = [Path.home() / relative / "Contents" /
                      f"mconnect-obsolete-probe-{uuid.uuid4().hex}.txt" for relative in BUNDLES]
            try:
                for attempt in range(2):
                    if attempt == 1:
                        for probe in probes:
                            probe.write_text("Temporary MConnect installer upgrade verification.\n")
                    subprocess.run(["/usr/sbin/installer", "-pkg", str(package), "-target",
                                    "CurrentUserHomeDirectory", "-verboseR"], check=True)
                    for kind, relative in zip(("au", "vst3"), BUNDLES):
                        payload = payloads[kind]
                        installed = Path.home() / relative
                        output("codesign", "--verify", "--deep", "--strict", installed)
                        for shipped in (payload / relative).rglob("*"):
                            if shipped.is_file():
                                actual = installed / shipped.relative_to(payload / relative)
                                require(actual.read_bytes() == shipped.read_bytes(), f"Installation differs: {actual}")
                require(all(not probe.exists() for probe in probes), "Upgrade left obsolete plugin files")
                print("PASS: repeat installation removes obsolete files in both plugin formats")
            finally:
                for probe in probes:
                    probe.unlink(missing_ok=True)
            for shipped in payloads["docs"].rglob("*"):
                if shipped.is_file():
                    actual = Path.home() / shipped.relative_to(payloads["docs"])
                    require(actual.read_bytes() == shipped.read_bytes(), f"Installed documentation differs: {actual}")
        print(f"PASS: MConnect {version} installer — home domain, universal AU/VST3, "
              "signatures, source, licenses and localization" + (", installed files" if install else ""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--install", action="store_true", help="Install into the current user's home and verify a repeat upgrade")
    arguments = parser.parse_args()
    verify(arguments.package.resolve(), arguments.install)
