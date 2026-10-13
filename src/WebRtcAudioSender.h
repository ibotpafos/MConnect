// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include "StreamTypes.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct mg_connection;
struct OpusEncoder;

namespace rtc
{
class DataChannel;
class PeerConnection;
class Track;
}

namespace mconnect
{
class WebRtcAudioSender
{
public:
    explicit WebRtcAudioSender(std::shared_ptr<juce::WaitableEvent> activity);
    ~WebRtcAudioSender();

    void applyConfig(const StreamConfig& newConfig);
    void handleOffer(mg_connection* connection, const juce::var& message);
    void handleRemoteCandidate(mg_connection* connection, const juce::var& message);
    void setReceiverReady(mg_connection* connection, bool ready);
    void updateReceiverStats(mg_connection* connection, const juce::var& message);
    void removeConnection(mg_connection* connection);
    void clear();

    void encodeAndSend(const float* interleavedStereo48k, size_t frameCount);
    void resetAudio();
    void skipAudio(size_t skippedFrames48k);

    int peerCount() const;
    int openTrackCount() const;
    int openOutputCount() const;
    bool hasOpenTrack() const;
    size_t opusFrameCount() const;
    size_t audioFrameCount() const;

    void fillStats(StreamStats& stats) const;

private:
    struct Peer;
    struct SendResult
    {
        uint64_t attempted = 0;
        uint64_t sent = 0;
        uint64_t failed = 0;
    };

    using PeerPtr = std::shared_ptr<Peer>;

    void configureEncoderIfNeeded();
    void recreateEncoder();
    void resetCountersLocked();
    void resetEncoderComplexityLocked();
    void fallbackEncoderComplexityLocked();
    SendResult sendEncodedFrameLocked(const uint8_t* data, size_t bytes, uint64_t frameStartSample);
    SendResult sendPcmFrameLocked(const float* interleavedStereo48k, size_t frameCount);
    void configureDataChannel(PeerPtr peer, std::shared_ptr<rtc::DataChannel> dataChannel);
    static void sendJson(mg_connection* connection, const juce::String& json);
    void closePeer(PeerPtr peer);
    PeerPtr findPeer(mg_connection* connection) const;

    static juce::String stateName(int state);
    static juce::String iceStateName(int state);

    mutable std::mutex mutex;
    std::shared_ptr<juce::WaitableEvent> activityEvent;
    StreamConfig config;
    std::unordered_map<mg_connection*, PeerPtr> peers;

    std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)> encoder;
    int encoderBitrateBps = 0;
    int encoderFrameMs = 0;
    int encoderComplexity = 5;
    size_t encoderFrameFrames = 960;
    std::vector<float> pendingPcm;
    size_t pendingFrames = 0;
    std::vector<float> pcmPending;
    size_t pcmPendingFrames = 0;
    std::vector<uint8_t> pcmPacket;
    std::vector<uint8_t> opusPacket;
    uint64_t encodedSampleCursor = 0;
    uint64_t pcmSampleCursor = 0;
    uint64_t pcmSequence = 0;
    uint32_t pcmStreamId = 1;
    bool pcmDiscontinuityPending = false;
    double lastReceiverReadyAckMs = 0.0;
    double pcmReceiverBufferMs = 0.0;
    uint64_t pcmReceiverUnderflows = 0;
    uint64_t pcmReceiverOverflows = 0;
    uint64_t pcmReceiverMissingPackets = 0;
    uint64_t pcmReceiverLatePackets = 0;
    double pcmAudioContextSampleRate = 0.0;
    double pcmAudioContextBaseLatencyMs = -1.0;
    double pcmAudioContextOutputLatencyMs = -1.0;
    juce::String pcmReceiverLastError;
    bool haveLastSubmittedTimestamp = false;
    uint32_t lastSubmittedTimestamp = 0;
    uint16_t currentSequenceNumber = 0;
    uint32_t currentTimestamp = 0;
    uint32_t currentTimestampIncrementActual = 0;
    uint32_t currentTimestampIncrementExpected = 960;
    uint32_t currentSsrc = 0;
    int negotiatedPayloadType = 111;
    double inputRmsL = 0.0;
    double inputRmsR = 0.0;
    double inputMidRms = 0.0;
    double inputSideRms = 0.0;
    double inputCorrelation = 0.0;
    unsigned meterFrameCounter = 0;

    std::atomic<uint64_t> encodedFrames { 0 };
    std::atomic<uint64_t> encodedPackets { 0 };
    std::atomic<uint64_t> encodedBytes { 0 };
    std::atomic<uint64_t> sendCalls { 0 };
    std::atomic<uint64_t> rtpPacketsAttempted { 0 };
    std::atomic<uint64_t> rtpPacketsSent { 0 };
    std::atomic<uint64_t> rtpSendFailures { 0 };
    std::atomic<uint64_t> encodeOverBudgetCount { 0 };
    std::atomic<uint64_t> timestampAnomalyCount { 0 };
    std::atomic<int> pcmOpenChannels { 0 };
    std::atomic<int> pcmReceiverReadyCount { 0 };
    std::atomic<uint64_t> pcmPacketsSent { 0 };
    std::atomic<uint64_t> pcmBytesSent { 0 };
    std::atomic<uint64_t> pcmSendCalls { 0 };
    std::atomic<uint64_t> pcmSendFailures { 0 };
    std::atomic<uint64_t> pcmDroppedBeforeSend { 0 };
    std::atomic<uint64_t> pcmReceiverStatsCount { 0 };
    std::atomic<uint64_t> opusEncodeErrors { 0 };
    std::atomic<uint64_t> opusForceChannelsErrors { 0 };
    std::atomic<uint64_t> opusMonoPackets { 0 };
    std::atomic<uint64_t> opusStereoPackets { 0 };
    std::atomic<uint64_t> opusPacketChannelErrors { 0 };
    std::atomic<int> opusLastEncodedChannels { 0 };
    std::atomic<uint64_t> opusPacketBytesTotal { 0 };
    std::atomic<int> opusPacketBytesLast { 0 };
};
}
