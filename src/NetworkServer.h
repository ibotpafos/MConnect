// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include "AudioTapFifo.h"
#include "NetworkInterfaceUtils.h"
#include "Resampler.h"
#include "StreamTypes.h"
#include "WebRtcAudioSender.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <string>
#include <vector>

struct mg_connection;
struct mg_context;

namespace mconnect
{
class NetworkServer final : private juce::Thread
{
public:
    explicit NetworkServer(AudioTapFifo& fifoToRead);
    ~NetworkServer() override;

    void applyConfig(const StreamConfig& newConfig);
    void setSessionSampleRate(double sampleRate, int blockFrames = 256);
    StreamStats getStats() const;
    juce::String getPairingUrl(const juce::String& address = {}) const;
    bool isListening() const noexcept { return receiversActive.load(std::memory_order_relaxed); }

private:
    void run() override;

    bool startServer(const StreamConfig& newConfig);
    void stopServer();
    void updateStatus(juce::String text);
    void refreshLanSelection(int port, bool useHttps);

    int handleHttpRequest(mg_connection* connection);
    bool isAuthorized(const mg_connection* connection) const;

    static int requestHandler(mg_connection* connection, void* cbdata);
    static int wsConnectHandler(const mg_connection* connection, void* cbdata);
    static void wsReadyHandler(mg_connection* connection, void* cbdata);
    static int wsDataHandler(mg_connection* connection, int bits, char* data, size_t dataLen, void* cbdata);
    static void wsCloseHandler(const mg_connection* connection, void* cbdata);
    static int logHandler(const mg_connection*, const char*);

    bool handleWebSocketText(mg_connection* connection, const char* data, size_t dataLen);
    void noteStateChangeLocked(const char* origin, const char* reason);

    AudioTapFifo& fifo;
    mutable std::mutex configMutex;
    StreamConfig selectedConfig;
    StreamConfig config;
    uint64_t stateRevision = 0;
    juce::String stateOrigin { "startup" };
    juce::String lastAdaptationReason { "startup" };
    juce::String lastAdaptationTimestamp;
    juce::String lastStateUpdateSentTimestamp;
    juce::String autonegotiationMode { "quality" };
    juce::String autonegotiationState { "inactive" };

    mutable std::mutex websocketMutex;
    std::unordered_set<mg_connection*> websocketConnections;

    mutable std::mutex statusMutex;
    juce::String statusText { "disabled" };
    juce::String lanUrl;
    juce::String currentLanIp;
    juce::String listenAddress { "0.0.0.0" };
    juce::StringArray candidateLanUrls;
    juce::StringArray candidateLanDescriptions;

    std::atomic<bool> enabled { false };
    std::atomic<bool> contextRunning { false };
    std::atomic<double> sessionSampleRate { 48000.0 };
    std::atomic<int> sourceBlockFrames {256};
    std::atomic<uint64_t> framesSent { 0 };
    std::atomic<uint64_t> serverFifoUnderruns { 0 };

    mg_context* context = nullptr;
    const juce::String pairingToken { juce::Uuid().toString() };
    std::atomic<int> listenPort {8123};
    std::atomic<bool> receiversActive {false};
    std::shared_ptr<juce::WaitableEvent> wakeEvent {std::make_shared<juce::WaitableEvent>()};
    std::atomic<uint64_t> workerWakeups {0}, backlogDiscardedFrames {0}, conversionErrors {0};
    WebRtcAudioSender webrtcSender;
    Resampler webrtcResampler;

    std::vector<float> readBuffer;
    std::vector<float> webrtcResampledBuffer;
    std::vector<float> webrtcSilenceBuffer;
};
}
