# MConnect 0.1.5 specification

## User outcome

Insert MConnect last on the master or listen bus, open the editor, scan the QR
code, and tap Listen in the phone browser. Listen to precisely the signal at the
insert point. Computer and phone must share a reachable local network. No
account, hosted service, phone app, microphone permission, or internet connection
is needed after installation.

## Scope and compatibility

- macOS: AU and VST3, Apple Silicon and Intel via a universal build.
- Windows: VST3 build configuration; Linux: VST3 and LV2 configuration.
- Modern iOS Safari and Android Chrome receive a native WebRTC audio track.
- Pro Tools AAX needs Avid's separately licensed SDK and is outside this build.
- Support mono and stereo buses; mono is duplicated for the phone.
- DAW output remains bit-identical with zero added plugin latency, including
  network failure, stop, bypass and offline export. Float/double host processing.
- DAW sample rates 32–192 kHz are converted on the worker to 48 kHz stereo Opus.
- Fixed 320 kbps stereo Opus, 10 ms frames. Opus is lossy.
- Up to four receivers; multiple instances get distinct ports and pairing links.
- A 420 × 540 editor: QR, connection status, small stereo meters, start/stop and
  copy link and a read-only version label. No manual network, codec, quality or buffer configuration.
- Automatic LAN address selection, free port selection, resampling, preconnection,
  playback readiness, temporary connection recovery and receiver buffer hints.
- Browser-only receiver; no separate mobile or desktop application in delivery.
- Closing the editor keeps transmission running. Disabling/unloading stops it.
- No promise of zero delay, operation on every historical phone/host, or reliable
  background/locked-screen playback; browser, Wi-Fi and Bluetooth add latency.

## Architecture

Extend PGStream 0.9.2 rather than rebuild the native network audio engine. JUCE 8
provides the host wrappers and native GUI. CivetWeb serves embedded assets and
same-origin WebSocket signaling. libdatachannel sends DTLS-SRTP media; libopus
encodes music with forced stereo and no voice processing. libsamplerate replaces
the upstream linear interpolation. Nayuki's QR library replaces the fixed-size
upstream QR implementation so full pairing links remain scannable.

Audio callback → bounded preallocated SPSC FIFO → worker SRC/Opus → WebRTC track
→ browser audio element. The producer owns only the write cursor and drops new
frames on overflow; the consumer owns the read cursor and discards old backlog.
No socket, lock, allocation or encoder runs in processBlock.
Capture/encoding stop when no receiver is ready for playback; meters run only
while the editor is open. Audio is encoded once and shared by all receivers.
The FIFO is fixed at 65,536 stereo frames (512 KiB); the worker bounds backlog to
40 ms or two host blocks, whichever is larger, and drops stale frames after stalls.
Opus starts at complexity 5 and can reduce to 3 if its worker budget is exceeded.
The sleeping network worker uses JUCE's high non-realtime priority (normal as a
fallback) so other CPU work does not unnecessarily defer small audio packets.
The fixed profile is cached per server run. SRC processes approximately 10 ms
chunks using the source clock; its state is allocated only when conversion is
needed. Idle workers wait up to 500 ms and wake on receiver/stop events.
RTP timestamps use integer 48 kHz sample positions, including discarded audio
and failed sends. Diagnostic RMS is calculated at 20 Hz. LAN enumeration takes
place while no receiver is active.

Receiving-only WebRTC works on a LAN HTTP page without getUserMedia or
AudioWorklet. A persistent audio element and explicit tap-to-play fallback handle
iOS autoplay. Audio transport is encrypted; HTTP signaling remains LAN-only.
The pairing token is fresh per plugin instance, not saved with the DAW session.
Only requests carrying that token can negotiate or read session diagnostics;
Origin is checked on WebSocket upgrades. No STUN/TURN servers are configured.
The page connects immediately and asks for one playback gesture only if autoplay
is denied. Media is gated until playback is requested, avoiding queued audio before
the user's tap. A broken session keeps retrying while listening is requested,
with backoff capped at 10 seconds. Stop cancels retries. Stale events, abandoned
HTTP requests and restored page-cache sessions cannot disable a newer session.
The page offers generic Opus NACK. The sender answers only feedback offered for
that audio payload and reuses libdatachannel's bounded 512-packet RTP cache;
retransmissions do not re-encode audio or run on the DAW callback. Receivers that
do not offer NACK continue to work without the cache.
After playback becomes ready, an NACK receiver gets a bounded 600 ms startup
reserve of encoded frames, sent in order before regular pacing. This seeds the
browser's actual packet buffer even if the link is already lossy when connecting.
The queue is preallocated per peer, limited to 60 packets of at most 4096 bytes,
and cancelled on Stop/readiness loss or source discontinuity. It runs only on the
network worker and adds a short startup wait, without delaying the DAW signal.
The page starts with a 300 ms receiver-buffer hint and automatically grows it up
to 600 ms for jitter, round-trip time, loss and concealment, using interval
statistics. Growth is fast; reduction is limited to 20 ms after 12 seconds of
healthy audio. No buffer or network controls are exposed. When supported it uses
`jitterBufferTarget`, falling back to `playoutDelayHint` and then the browser's
own buffering. These hints do not guarantee end-to-end latency or uninterrupted
audio on an arbitrarily poor network.
Signaling rejects malformed, oversized, fragmented, binary and deeply nested
JSON messages. Concurrent offers cannot exceed four native peers.

## Update ownership

GD Connect owns release discovery, downloads and installation. The plugin has
no update service, release polling, download transport or Installer handoff.
The manager remains optional for audio streaming; manual PKG/ZIP installation
is supported. Keep the AU/VST3 bundle ID `audio.mconnect.plugin`, plugin codes,
version metadata and universal ZIP release contract stable. Close the DAW before
replacing installed bundles. Earlier 0.1.3/0.1.4 clients can discover the transition
release through its PKG asset.

## Delivery plan and acceptance

1. Research upstream, licenses and current APIs with Context7; record sources.
2. Adapt portable build, safe capture, pairing and UI; embed source/license files.
3. Compile universal macOS AU/VST3 and an integration harness.
4. Verify bit-identical passthrough, stereo/mono/double, overflow concurrency,
   sample rate conversion, pairing rejection, multiple instances and cleanup.
5. Run real browser receive/reconnect tests; decode received audio and compare
   left/right test tones, including 44.1/48/96/192 kHz input and DAW idle.
6. Validate AU with auval and provide install/build instructions and artifacts.

Device/DAW checks unavailable locally remain explicitly listed as unverified.
