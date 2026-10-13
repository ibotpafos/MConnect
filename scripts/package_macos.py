#!/usr/bin/env python3
"""Build a current-user macOS Installer package and a manual-install ZIP."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parent.parent
LICENSE_FILES = ("LICENSE", "NOTICE.md", "THIRD_PARTY_NOTICES.md", "LICENSES_THIRD_PARTY.md")


def run(*arguments):
    subprocess.run([str(argument) for argument in arguments], check=True)


def check_sources(build, workspace):
    """Catch packaging source that has not yet been embedded in the binaries."""
    fresh = workspace / "source-current.zip"
    run(sys.executable, ROOT / "scripts/source_archive.py", ROOT, fresh)
    with zipfile.ZipFile(fresh) as expected, zipfile.ZipFile(build / "generated/source.zip") as actual:
        if set(expected.namelist()) != set(actual.namelist()) or any(
            expected.read(name) != actual.read(name) for name in expected.namelist()
        ):
            raise SystemExit("Source archive is stale. Run cmake --build before packaging.")


def stage_plugins(build, stage, application_identity):
    versions = set()
    for kind, extension in (("AU", "component"), ("VST3", "vst3")):
        source = build / "MConnect_artefacts/Release" / kind / f"MConnect.{extension}"
        with (source / "Contents/Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        versions.add(info["CFBundleShortVersionString"])
        executable = source / "Contents/MacOS" / info["CFBundleExecutable"]
        architectures = subprocess.check_output(["lipo", "-archs", str(executable)], text=True).split()
        if set(architectures) != {"arm64", "x86_64"}:
            raise SystemExit(f"Expected universal arm64 + x86_64 binary: {executable}")
        if (build / "generated/source.zip").read_bytes() not in executable.read_bytes():
            raise SystemExit(f"Embedded source is stale in {executable}; rebuild this format.")
        if any(path.suffix in (".app", ".dylib") for path in source.rglob("*")):
            raise SystemExit(f"Unexpected nested executable in {source}")
        target = stage / source.name
        run("ditto", source, target)
        sign = ["codesign", "--force", "--sign", application_identity or "-"]
        if application_identity:
            sign += ["--options", "runtime", "--timestamp"]
        run(*sign, target)
        run("codesign", "--verify", "--deep", "--strict", target)
    if len(versions) != 1:
        raise SystemExit("AU and VST3 versions differ; rebuild both formats.")
    version = versions.pop()
    project_version = re.search(r"project\(MConnect VERSION ([\d.]+)",
                                (ROOT / "CMakeLists.txt").read_text()).group(1)
    if version != project_version or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit("Plugin version does not match the project version; rebuild first.")
    return version


def create_installer(stage, workspace, version, installer_identity):
    packages = []
    for directory, extension in (("Components", "component"), ("VST3", "vst3")):
        kind = "au" if extension == "component" else "vst3"
        payload = workspace / f"payload-{kind}"
        relative = Path("Library/Audio/Plug-Ins") / directory / f"MConnect.{extension}"
        target = payload / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        run("ditto", stage / target.name, target)
        components = workspace / f"components-{kind}.plist"
        with components.open("wb") as stream:
            plistlib.dump([
                {"RootRelativeBundlePath": relative.as_posix(), "BundleIsRelocatable": False,
                 "BundleIsVersionChecked": True, "BundleHasStrictIdentifier": True,
                 "BundleOverwriteAction": "upgrade"}
            ], stream)
        component = workspace / f"MConnect-{kind}.pkg"
        # Each format gets its own receipt: JUCE uses the same bundle ID for AU/VST3.
        # Two equal bundle IDs in one PackageInfo make Installer skip VST3 cleanup.
        run("pkgbuild", "--root", payload, "--identifier", f"audio.mconnect.pkg.{kind}",
            "--version", version, "--install-location", "/", "--ownership", "recommended",
            "--component-plist", components, component)
        packages.append(component)

    payload = workspace / "payload-docs"
    documentation = payload / "Library/Application Support/MConnect"
    documentation.mkdir(parents=True)
    for name in (*LICENSE_FILES, "README.md", "CHANGELOG.md", "MConnect-source.zip"):
        shutil.copyfile(stage / name, documentation / name)
    shutil.copytree(stage / "docs", documentation / "docs")

    component = workspace / "MConnect-docs.pkg"
    run("pkgbuild", "--root", payload, "--identifier", "audio.mconnect.pkg.docs",
        "--version", version, "--install-location", "/", "--ownership", "recommended", component)
    packages.append(component)

    requirements = workspace / "requirements.plist"
    with requirements.open("wb") as stream:
        plistlib.dump({"os": ["11.0"], "arch": ["arm64", "x86_64"], "home": True}, stream)
    distribution_path = workspace / "Distribution"
    command = ["productbuild", "--synthesize", "--product", requirements]
    for component in packages:
        command += ["--package", component]
    run(*command, distribution_path)
    distribution = ET.parse(distribution_path)
    gui = distribution.getroot()
    gui.find("domains").attrib.update(enable_anywhere="false", enable_localSystem="false",
                                     enable_currentUserHome="true")
    ET.SubElement(gui, "title").text = f"MConnect {version}"
    ET.SubElement(gui, "product", id="audio.mconnect.installer", version=version)
    for section, filename, mime in (("welcome", "welcome.html", "text/html"),
                                   ("license", "license.txt", "text/plain"),
                                   ("conclusion", "conclusion.html", "text/html")):
        ET.SubElement(gui, section, {"file": filename, "mime-type": mime})
    ET.indent(distribution)
    distribution.write(distribution_path, encoding="utf-8", xml_declaration=True)

    resources = workspace / "resources"
    shutil.copytree(ROOT / "assets/installer", resources)
    shutil.copyfile(ROOT / "LICENSE", resources / "license.txt")
    product = workspace / f"MConnect-{version}-macos-universal.pkg"
    command = ["productbuild", "--distribution", distribution_path, "--resources", resources,
               "--package-path", workspace]
    if installer_identity:
        command += ["--sign", installer_identity, "--timestamp"]
    run(*command, product)
    return product


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", nargs="?", type=Path, default=ROOT / "build")
    parser.add_argument("--application-identity", help="Developer ID Application identity")
    parser.add_argument("--installer-identity", help="Developer ID Installer identity")
    arguments = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("macOS packaging requires Apple's pkgbuild and productbuild.")
    if bool(arguments.application_identity) != bool(arguments.installer_identity):
        parser.error("Supply both Developer ID identities, or neither for a local build.")
    build = arguments.build.resolve()
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mconnect-package-", dir=output) as directory:
        workspace = Path(directory)
        check_sources(build, workspace)
        stage = workspace / "manual"
        stage.mkdir()
        version = stage_plugins(build, stage, arguments.application_identity)
        for name in ("README.md", "CHANGELOG.md", *LICENSE_FILES):
            shutil.copyfile(ROOT / name, stage / name)
        shutil.copytree(ROOT / "docs", stage / "docs")
        if (ROOT / "test-results").exists():
            shutil.copytree(ROOT / "test-results", stage / "test-results")
        shutil.copyfile(ROOT / "scripts/Install.command", stage / "Install.command")
        (stage / "Install.command").chmod(0o755)
        shutil.copyfile(build / "generated/source.zip", stage / "MConnect-source.zip")
        product = create_installer(stage, workspace, version, arguments.installer_identity)
        run(sys.executable, ROOT / "tests/installer.py", product)

        basename = f"MConnect-{version}-macos-universal"
        expanded = output / basename
        # Publish a clean tree so removed files cannot leak into a new package.
        if expanded.exists():
            shutil.rmtree(expanded)
        stage.replace(expanded)
        archive = output / f"{basename}.zip"
        run("ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", expanded, archive)
        final_product = output / product.name
        product.replace(final_product)
        checksums = output / f"{basename}.sha256"
        entries = []
        for path in (final_product, archive):
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            entries.append(f"{digest}  {path.name}\n")
        checksums.write_text("".join(entries))
        print(f"Installer: {final_product}\nManual ZIP: {archive}\nSHA-256: {checksums}")


if __name__ == "__main__":
    main()
