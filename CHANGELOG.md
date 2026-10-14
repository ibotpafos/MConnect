# Changelog

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
