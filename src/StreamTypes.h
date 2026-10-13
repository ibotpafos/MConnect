// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mconnect
{
enum class OpusBitrateMode : int
{
    goodPreview128 = 0,
    veryGood192 = 1,
    studioPreview256 = 2,
    highQuality320 = 3,
    experimentalMax510 = 4
};

enum class LatencyMode : int
{
    ultraLowExperimental = 0,
    lowLatency = 1,
    medium = 2,
    safe = 3,
    verySafe = 4
};

enum class TransportMode : int
{
    opusWebRtc = 0,
    pcm16DataChannel = 1,
    pcm24DataChannel = 2,
    pcm32FloatDataChannel = 3,
    autoSelect = 4
};

struct StreamConfig
{
    bool streamEnabled = false;
    int port = 8123;
    TransportMode selectedTransportMode = TransportMode::opusWebRtc;
    TransportMode transportMode = TransportMode::opusWebRtc;
    OpusBitrateMode opusBitrateMode = OpusBitrateMode::highQuality320;
    LatencyMode latencyMode = LatencyMode::medium;
    bool keepAliveWhenIdle = true;
    bool useSelfSignedCertificate = true;
    bool audioPassthrough = true;
    double sessionSampleRate = 48000.0;

    static TransportMode resolveTransportMode(TransportMode selectedMode, bool httpsEnabled)
    {
        if (! httpsEnabled
            && (selectedMode == TransportMode::pcm16DataChannel
                || selectedMode == TransportMode::pcm24DataChannel
                || selectedMode == TransportMode::pcm32FloatDataChannel
                || selectedMode == TransportMode::autoSelect))
            return TransportMode::opusWebRtc;

        if (selectedMode == TransportMode::autoSelect)
            return TransportMode::pcm32FloatDataChannel;

        return selectedMode;
    }

    bool usesHttps() const
    {
        return useSelfSignedCertificate;
    }

    bool usesPcmDataChannel() const
    {
        return transportMode == TransportMode::pcm16DataChannel
            || transportMode == TransportMode::pcm24DataChannel
            || transportMode == TransportMode::pcm32FloatDataChannel;
    }

    int pcmBitsPerSample() const
    {
        if (transportMode == TransportMode::pcm16DataChannel)
            return 16;
        if (transportMode == TransportMode::pcm24DataChannel)
            return 24;
        return 32;
    }

    int pcmCodecId() const
    {
        if (! usesPcmDataChannel())
            return 0;
        if (transportMode == TransportMode::pcm16DataChannel)
            return 1;
        if (transportMode == TransportMode::pcm24DataChannel)
            return 2;
        return 3;
    }

    int pcmPacketFrameCount() const
    {
        return pcmPacketFrameCount(48000);
    }

    int pcmPacketFrameCount(int sampleRate) const
    {
        return framesForDurationMs(sampleRate, pcmPacketDurationMs());
    }

    int pcmPacketDurationMs() const
    {
        if (latencyMode == LatencyMode::medium)
            return 20;
        if (latencyMode == LatencyMode::safe)
            return 40;
        if (latencyMode == LatencyMode::verySafe)
            return 100;
        return 10;
    }

    int pcmTargetBufferMs() const
    {
        if (latencyMode == LatencyMode::medium)
            return 100;
        if (latencyMode == LatencyMode::safe)
            return 180;
        if (latencyMode == LatencyMode::verySafe)
            return 300;
        return 60;
    }

    int pcmResumeBufferMs() const
    {
        return pcmTargetBufferMs();
    }

    int pcmRingCapacityMs() const
    {
        if (latencyMode == LatencyMode::medium)
            return 400;
        if (latencyMode == LatencyMode::safe)
            return 700;
        if (latencyMode == LatencyMode::verySafe)
            return 1000;
        return 250;
    }

    juce::String serverScheme() const
    {
        return usesHttps() ? "https" : "http";
    }

    juce::String selectedTransportModeName() const
    {
        if (selectedTransportMode == TransportMode::pcm16DataChannel)
            return "PCM16 DataChannel";
        if (selectedTransportMode == TransportMode::pcm24DataChannel)
            return "PCM24 DataChannel";
        if (selectedTransportMode == TransportMode::pcm32FloatDataChannel)
            return "PCM32F DataChannel";
        if (selectedTransportMode == TransportMode::autoSelect)
            return "Auto";
        return "WebRTC Opus";
    }

    juce::String transportModeName() const
    {
        if (transportMode == TransportMode::pcm16DataChannel)
            return "PCM16 DataChannel";
        if (transportMode == TransportMode::pcm24DataChannel)
            return "PCM24 DataChannel";
        if (transportMode == TransportMode::pcm32FloatDataChannel)
            return "PCM32F DataChannel";
        return "WebRTC Opus";
    }

    int opusBitrateBps() const
    {
        if (opusBitrateMode == OpusBitrateMode::goodPreview128)
            return 128000;
        if (opusBitrateMode == OpusBitrateMode::veryGood192)
            return 192000;
        if (opusBitrateMode == OpusBitrateMode::studioPreview256)
            return 256000;
        if (opusBitrateMode == OpusBitrateMode::experimentalMax510)
            return 510000;
        return 320000;
    }

    juce::String opusBitrateName() const
    {
        if (opusBitrateMode == OpusBitrateMode::goodPreview128)
            return "128 kb/s Good Preview";
        if (opusBitrateMode == OpusBitrateMode::veryGood192)
            return "192 kb/s Very Good";
        if (opusBitrateMode == OpusBitrateMode::studioPreview256)
            return "256 kb/s Studio Preview";
        if (opusBitrateMode == OpusBitrateMode::experimentalMax510)
            return "510 kb/s Experimental Max";
        return "320 kb/s High Quality / Recommended";
    }

    juce::String latencyModeName() const
    {
        if (latencyMode == LatencyMode::ultraLowExperimental)
            return "Ultra Low";
        if (latencyMode == LatencyMode::lowLatency)
            return "Low";
        if (latencyMode == LatencyMode::medium)
            return "Medium";
        if (latencyMode == LatencyMode::safe)
            return "Safe";
        return "Very Safe";
    }

    juce::String latencyTargetDescription() const
    {
        if (latencyMode == LatencyMode::medium)
            return "100 ms PCM target fill";
        if (latencyMode == LatencyMode::safe)
            return "180 ms PCM target fill";
        if (latencyMode == LatencyMode::verySafe)
            return "300 ms PCM target fill";
        return "60 ms PCM target fill";
    }

    int playoutDelayHintMs() const
    {
        if (latencyMode == LatencyMode::safe)
            return 150;
        if (latencyMode == LatencyMode::lowLatency || latencyMode == LatencyMode::ultraLowExperimental)
            return 30;
        if (latencyMode == LatencyMode::verySafe)
            return 180;
        return 80;
    }

    int opusFrameDurationMs() const
    {
        if (latencyMode == LatencyMode::lowLatency)
            return 10;
        if (latencyMode == LatencyMode::ultraLowExperimental)
            return 10;
        return 20;
    }

private:
    static int framesForDurationMs(int sampleRate, int durationMs)
    {
        const auto safeRate = juce::jmax(1, sampleRate);
        const auto safeMs = juce::jmax(1, durationMs);
        return static_cast<int> ((static_cast<int64_t> (safeRate) * safeMs + 500) / 1000);
    }
};

struct StreamStats
{
    bool serverRunning = false;
    bool streamEnabled = false;
    int port = 8123;
    int connectedClients = 0;
    int webrtcPeerCount = 0;
    int webrtcOpenTracks = 0;
    juce::String lanUrl;
    juce::String currentLanIp;
    juce::String listenAddress;
    juce::String statusText;
    juce::String transportMode;
    juce::String selectedTransportMode;
    juce::String serverScheme;
    bool httpsEnabled = true;
    bool selfSignedCertificateEnabled = true;
    bool audioPassthrough = true;
    int pcmBitsPerSample = 0;
    int pcmPacketFrames = 960;
    int pcmPacketDurationMs = 20;
    int pcmTargetBufferMs = 100;
    int pcmResumeBufferMs = 100;
    int pcmRingCapacityMs = 400;
    int inputSampleRate = 48000;
    int selectedOpusBitrateBps = 320000;
    juce::String selectedOpusBitratePreset;
    int opusBitrateBps = 320000;
    juce::String opusBitratePreset;
    juce::String opusCodec { "opus/48000/2" };
    bool opusBitrateLimited = false;
    juce::String selectedLatencyMode;
    juce::String latencyMode;
    juce::String latencyTarget;
    int opusFrameDurationMs = 20;
    int activePlayoutDelayHintMs = 80;
    bool autonegotiationEnabled = false;
    juce::String autonegotiationMode;
    juce::String autonegotiationState;
    juce::String lastAdaptationReason;
    juce::String lastAdaptationTimestamp;
    uint64_t stateRevision = 0;
    juce::String stateOrigin;
    juce::String lastUserRequestedBitratePreset;
    juce::String lastUserRequestedLatencyMode;
    juce::String lastStateUpdateSentTimestamp;
    uint64_t framesSent = 0;
    uint64_t serverFifoUnderruns = 0;
    uint64_t fifoDroppedFrames = 0;
    size_t senderQueueFillFrames = 0;
    size_t senderQueueCapacityFrames = 0;
    double senderQueueFillMs = 0.0;
    uint64_t webrtcEncodedFrames = 0;
    uint64_t webrtcEncodedPackets = 0;
    uint64_t webrtcEncodedBytes = 0;
    uint64_t webrtcSendCalls = 0;
    uint64_t webrtcRtpPacketsAttempted = 0;
    uint64_t webrtcRtpPacketsSent = 0;
    uint64_t webrtcRtpSendFailures = 0;
    uint64_t webrtcEncodeOverBudgetCount = 0;
    uint64_t webrtcEncoderOverloadWarnings = 0;
    int webrtcNegotiatedPayloadType = 111;
    int webrtcActualPayloadType = 111;
    uint32_t webrtcSsrc = 0;
    uint16_t webrtcSequenceCurrent = 0;
    uint32_t webrtcTimestampCurrent = 0;
    uint32_t webrtcTimestampIncrementExpected = 960;
    uint32_t webrtcTimestampIncrementActual = 0;
    uint64_t webrtcTimestampAnomalyCount = 0;
    uint64_t webrtcPacketsSubmittedToTrack = 0;
    uint64_t webrtcBytesSubmittedToTrack = 0;
    uint64_t webrtcSubmitErrors = 0;
    int pcmOpenChannels = 0;
    int pcmReceiverReadyCount = 0;
    uint64_t pcmPacketsSent = 0;
    uint64_t pcmBytesSent = 0;
    uint64_t pcmSendCalls = 0;
    uint64_t pcmSendFailures = 0;
    uint64_t pcmDroppedBeforeSend = 0;
    uint64_t pcmDataChannelBufferedBytes = 0;
    uint64_t pcmSequenceCurrent = 0;
    uint64_t pcmSampleCursor = 0;
    uint64_t pcmReceiverStatsCount = 0;
    double pcmReceiverBufferMs = 0.0;
    double pcmReceiverAckAgeMs = -1.0;
    uint64_t pcmReceiverUnderflows = 0;
    uint64_t pcmReceiverOverflows = 0;
    uint64_t pcmReceiverMissingPackets = 0;
    uint64_t pcmReceiverLatePackets = 0;
    double pcmAudioContextSampleRate = 0.0;
    double pcmAudioContextBaseLatencyMs = -1.0;
    double pcmAudioContextOutputLatencyMs = -1.0;
    juce::String pcmReceiverLastError;
    uint64_t opusEncodeErrors = 0;
    uint64_t opusForceChannelsErrors = 0;
    uint64_t opusMonoPackets = 0;
    uint64_t opusStereoPackets = 0;
    uint64_t opusPacketChannelErrors = 0;
    int opusLastEncodedChannels = 0;
    int opusPacketBytesLast = 0;
    double opusPacketBytesAvg = 0.0;
    double inputRmsL = 0.0;
    double inputRmsR = 0.0;
    double inputMidRms = 0.0;
    double inputSideRms = 0.0;
    double inputCorrelation = 0.0;
    juce::String webrtcConnectionState;
    juce::String webrtcIceConnectionState;
    juce::StringArray candidateLanUrls;
    juce::StringArray candidateLanDescriptions;
};
}
