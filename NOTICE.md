# MConnect attribution

MConnect is a derivative of [Andrulius/PGStream](https://github.com/Andrulius/PGStream),
upstream revision `e2080e92db4ba81afb24e5f006d67fd14da969b2` (PGStream 0.9.2).
The audio sender, server, state definitions and network discovery were adapted
from that project. Upstream copyright and AGPL-3.0-only licensing are preserved.

MConnect adds macOS AU/VST3 and portable builds, a new native editor and mobile
receiver, per-instance pairing, automatic port selection, safe FIFO ownership,
band-limited resampling, and verification tools. The implementation is licensed
under AGPL-3.0-only, including the JUCE AGPL option. See LICENSE.

Dependency revisions and licenses are documented in THIRD_PARTY_NOTICES.md and
dependencies.lock.json. The receiver offers the source archive from the plugin
itself at /source.zip. Build scripts fetch the pinned dependency sources.
