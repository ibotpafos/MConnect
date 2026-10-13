# Research and decisions — 2026-10-13

| Candidate | Fit | Decision |
| --- | --- | --- |
| [PGStream](https://github.com/Andrulius/PGStream) | Exact master-bus → QR → phone-browser workflow, JUCE/WebRTC/Opus; upstream Windows-only, AGPL-3.0-only | Extend its engine and replace UI/build/capture hazards |
| [SonoBus](https://github.com/sonosaurus/sonobus) | Mature native AU/VST app, Opus/PCM, GPLv3; expects its own receiver app | Useful reference; does not meet browser-only requirement |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) | Maintained portable C++ WebRTC media transport, MPL-2.0 | Reuse through PGStream, pinned release |
| [Waves Stream](https://www.waves.com/plugins/waves-stream) | Commercial plugin and browser service with QR workflow | Confirms workflow; cannot wrap its proprietary implementation |

Context7 resolved `/juce-framework/juce` and `/paullouisageneau/libdatachannel`.
Queried JUCE CMake plugin formats and libdatachannel Opus RTP sender examples.
Dependencies are pinned to exact revisions rather than mutable main branches.
No external account or service setup is required.

Browser design is based on primary evidence:

- [WebKit RTCPeerConnection IDL](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/Modules/mediastream/RTCPeerConnection.idl)
  and [Chromium IDL](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/peerconnection/rtc_peer_connection.idl):
  RTCPeerConnection is not SecureContext-gated. Receiving needs no microphone.
- [WebKit autoplay policies](https://webkit.org/blog/6784/new-video-policies-for-ios/)
  and [WebRTC playback policies](https://webkit.org/blog/7763/a-closer-look-into-webrtc/):
  an explicit audio play gesture may be needed after async negotiation.
- [Chrome autoplay](https://developer.chrome.com/blog/autoplay/): handle rejection
  of play() and expose a fresh tap-to-play action.
- [Web Audio](https://www.w3.org/TR/webaudio-1.0/): AudioWorklet is secure-context-only;
  native WebRTC audio avoids self-signed certificate setup and custom PCM playback.
- [QR generator](https://www.nayuki.io/page/qr-code-generator-library): use the MIT
  implementation instead of expanding a custom Reed–Solomon implementation.
- [libsamplerate](https://github.com/libsndfile/libsamplerate): streaming sinc SRC
  avoids aliasing of the upstream linear resampler when downsampling high-rate DAW audio.
- [Receiver buffer hints](https://developer.mozilla.org/en-US/docs/Web/API/RTCRtpReceiver/jitterBufferTarget)
  are feature-detected and adapted automatically. They are advisory;
  [jitter buffer statistics](https://developer.mozilla.org/en-US/docs/Web/API/RTCInboundRtpStreamStats/jitterBufferTargetDelay)
  do not measure complete acoustic latency.

These establish design feasibility, not physical-device certification. Real phone
and individual DAW compatibility still need a device matrix after local tests.
The user confirmed iPhone playback during development; final scope and checks
are recorded in [VALIDATION.md](VALIDATION.md).
