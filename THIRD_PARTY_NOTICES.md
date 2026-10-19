# Dependency notices

MConnect is AGPL-3.0-only, derived from Andrulius/PGStream. See NOTICE.md and LICENSE.
The full license texts for the source dependencies are in LICENSES_THIRD_PARTY.md.

| Dependency | Version | License |
| --- | --- | --- |
| JUCE | 8.0.14 | AGPLv3 option |
| VST3 SDK bundled with JUCE | JUCE's pinned tree | MIT |
| CivetWeb | 1.16 | MIT |
| Mbed TLS | 3.6.6 | Apache-2.0 option |
| libdatachannel | 0.24.1 | MPL-2.0 |
| Opus | 1.6.1 | BSD-style Opus license |
| libsamplerate | 0.2.2 | BSD-2-Clause |
| Nayuki QR Code generator | 1.8.0 | MIT |
| GD Kit | 0.1.0, pinned source archive in cmake/gdkit | This product's license and the included GD Kit NOTICE.md |

Exact source URLs and commit revisions: dependencies.lock.json. Recursive
submodules are pinned by their parent revisions: libjuice (MPL-2.0), libsrtp
(BSD), usrsctp (BSD), plog (MIT), and nlohmann/json (MIT). Dependency source trees
are downloaded into external/ by scripts/bootstrap.py and statically linked.
No external dylib installation is required by the macOS plugin.

PGStream's custom QR and linear resampler are replaced by qrcodegen and
libsamplerate. The mobile receiver and native editor are new MConnect assets.
The browser source archive includes all project modifications, dependency pins,
build scripts, licensing and research notes.

GD Kit — shared design and editor components for the GD plugin family.
Copyright 2026 GD Music. It builds on the JUCE modules already used by this
product; it does not include or relicense JUCE, its dependencies, or product DSP.
The complete pinned GD Kit sources and their notice are included in
`cmake/gdkit/GDKit-0.1.0.tar.gz` in the corresponding source archive.
