# Changelog

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
