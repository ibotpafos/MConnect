# Changelog

## 0.1.9 — 2026-10-19

- Negotiate Opus NACK and retransmit cached RTP packets without re-encoding audio.
- Seed a bounded encoded-audio reserve when playback starts so already lossy links can build a real receiver buffer.
- Automatically adapt the phone receiver's buffer from 300 to 600 ms using jitter, round-trip time, loss and concealment, without new controls.
- Add real UDP loss/jitter tests with decoded stereo and feedback-negotiation compatibility checks.
- Preserve the published Link name, GD Connect manufacturer and existing AU/VST3 identities.
- Include the pinned GD Kit editor theme and preserve existing verified signatures when packaging.

Versions 0.1.6–0.1.8 were distributed through the GD Connect channel for the
manufacturer, persistent beta signature and Link display name. Version 0.1.9
continues that sequence in both the product repository and the shared channel.

## 0.1.5 — 2026-10-18

- Centralize plugin updates in GD Connect; remove in-plugin checks, downloads and Installer prompts.
- Keep a read-only version label in the existing editor and remove the unused update transport and JUCE cryptography module.
- Preserve bundle identifiers, universal ZIP metadata and manual PKG installation for manager compatibility and older clients.
- Keep the editor lifecycle regression independent of the removed updater suite.
- Restore the macOS 11 deployment target when CMake has an empty cache entry; reject packages whose binaries require a newer OS.

## 0.1.4 — 2026-10-15

- Allow slower installer downloads with up to 60 seconds between chunks and a 10-minute transfer limit.
- Automatically retry transient download failures up to twice with cancellable backoff.
- Resume interrupted downloads when macOS provides native resume data; verify the complete file before use.
- Keep integrity failures and permanent HTTP errors visible without repeated downloads.
- Check free cache/temporary space before downloading and surface storage failures.
- Remove the Installer agreement step while keeping all license files and notices.
- Add an opt-in live editor-to-Installer test host and current-version regression checks.

## 0.1.3 — 2026-10-15

- Add quiet daily update checks and manual checks in the existing plugin editor.
- Download the macOS installer on request and verify its size and GitHub SHA-256 digest before offering it.
- Cache release information across sessions, keep known updates available offline and cancel requests on editor close.
- Use the system macOS HTTPS transport without an updater app or background daemon.
- Prepare a verified GitHub Releases publication workflow, with a local preview and explicit upload/publish modes.
- Keep the existing interface and audio processing unchanged.

## 0.1.2 — 2026-10-14

- Preserve exact normalized Stream automation values in saved state, with support for earlier presets.
- Use immutable AES tables and initialize hardware capability caches before RTC threads start.
- Enable Mbed TLS synchronization on POSIX and remove unused self-test counters from the plugin.
- Use atomic HTTP-server shutdown with a reviewed patch applied to a generated CivetWeb copy.
- Increase the browser buffer on loss/concealment even with low jitter, then reduce it after recovery.
- Bind WebRTC media to the selected LAN interface so ICE cannot route local audio through a VPN interface.
- Synchronize stored callback assignment/reset in libdatachannel with a generated header patch and a concurrent regression test.
- Add sustained four-receiver audio tests and sanitizer profiles for concurrent reconnect and shutdown.

## 0.1.1 — 2026-10-14

- Reduce audio capture, metering and encoding work when no listener is ready.
- Bound queued audio, batch resampling and reset resampler state on rate changes.
- Restore interrupted phone connections automatically and handle stale browser events.
- Harden WebSocket validation and concurrent listener limits.
- Verify stereo decoding, reconnection, CPU usage and plugin lifecycle without launching a DAW.
- Package universal macOS AU/VST3 in a standard Installer package for the current user.

## 0.1.0 — 2026-10-13

- Add the MConnect AU/VST3 plugin and phone browser receiver based on PGStream.
- Pair over the local network using a QR code and a per-instance connection key.
- Stream stereo Opus audio to up to four listeners while preserving DAW audio unchanged.
- Include pinned dependencies, license notices and corresponding source in the plugin.
