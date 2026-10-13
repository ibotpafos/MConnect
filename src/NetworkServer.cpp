// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "NetworkServer.h"
#include "HtmlAssets.h"
#include "SelfSignedCertificate.h"
#include <civetweb.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace mconnect
{
namespace
{
juce::String jsonPropertyString(const juce::var& object, const char* name)
{
    if (auto* dynamic = object.getDynamicObject())
        return dynamic->getProperty(name).toString();

    return {};
}

bool hasBoundedJsonNesting(const char* data, size_t size)
{
    // JUCE's parser is recursive. Bound its stack use before parsing a network
    // message; braces inside SDP strings are ordinary string content.
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i)
    {
        const auto c = data[i];
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
        else if (c == '\'') return false; // Standard JSON uses double quotes.
        else if (c == '{' || c == '[') { if (++depth > 8) return false; }
        else if (c == '}' || c == ']') { if (depth == 0) return false; --depth; }
    }
    return !quoted && depth == 0;
}

void sendHttpOkHeaders(mg_connection* connection, const char* mimeType, size_t size)
{
    auto header = juce::String("HTTP/1.1 200 OK\r\nContent-Type: ")
        + mimeType
        + "\r\nContent-Length: "
        + juce::String(static_cast<int64_t> (size))
        + "\r\nCache-Control: no-store"
        + "\r\nReferrer-Policy: no-referrer"
        + "\r\nX-Content-Type-Options: nosniff"
        + "\r\nContent-Security-Policy: default-src 'self'; img-src 'self' data:; connect-src 'self' ws: wss:; media-src 'self' blob:; style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"
        + "\r\nCross-Origin-Resource-Policy: same-origin"
        + "\r\n\r\n";

    mg_write(connection, header.toRawUTF8(), header.getNumBytesAsUTF8());
}

juce::String nowTimestamp()
{
    return juce::Time::getCurrentTime().toISO8601(true);
}
}

NetworkServer::NetworkServer(AudioTapFifo& fifoToRead)
    : juce::Thread("MConnect network worker"), fifo(fifoToRead), webrtcSender(wakeEvent)
{
    readBuffer.resize(8192);
    webrtcSilenceBuffer.resize(1920);
}

NetworkServer::~NetworkServer()
{
    stopServer();
}

void NetworkServer::setSessionSampleRate(double sampleRate, int blockFrames)
{
    if (std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0)
        sessionSampleRate.store(sampleRate, std::memory_order_release);
    sourceBlockFrames.store(juce::jlimit(1, 65536, blockFrames), std::memory_order_release);
}

void NetworkServer::applyConfig(const StreamConfig& newConfig)
{
    auto adjustedConfig = newConfig;
    adjustedConfig.port = listenPort.load(std::memory_order_acquire);
    adjustedConfig.sessionSampleRate = sessionSampleRate.load(std::memory_order_acquire);
    webrtcSender.applyConfig(adjustedConfig);

    bool needsRestart = false;
    auto changed = false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        needsRestart = contextRunning.load(std::memory_order_acquire)
            && adjustedConfig.streamEnabled
            && (adjustedConfig.port != selectedConfig.port
                || adjustedConfig.useSelfSignedCertificate != selectedConfig.useSelfSignedCertificate);
        changed = stateRevision == 0
            || adjustedConfig.opusBitrateMode != config.opusBitrateMode
            || adjustedConfig.latencyMode != config.latencyMode
            || adjustedConfig.port != config.port
            || adjustedConfig.selectedTransportMode != config.selectedTransportMode
            || adjustedConfig.transportMode != config.transportMode
            || adjustedConfig.useSelfSignedCertificate != config.useSelfSignedCertificate
            || adjustedConfig.keepAliveWhenIdle != config.keepAliveWhenIdle
            || adjustedConfig.audioPassthrough != config.audioPassthrough;
        selectedConfig = adjustedConfig;
        config = adjustedConfig;
        if (changed)
            noteStateChangeLocked(stateRevision == 0 ? "startup" : "plugin_user",
                                  stateRevision == 0 ? "initial active state" : "plugin parameter change");
    }

    if (!adjustedConfig.streamEnabled)
    {
        enabled.store(false, std::memory_order_release);
        webrtcSender.clear();
        stopServer();
        updateStatus("disabled");
        return;
    }

    if (contextRunning.load(std::memory_order_acquire) && ! needsRestart)
    {
        enabled.store(true, std::memory_order_release);
        return;
    }

    stopServer();
    if (startServer(adjustedConfig))
    {
        enabled.store(true, std::memory_order_release);
        webrtcResampler.reset();
        webrtcSender.resetAudio();
        // Small audio packets need timely scheduling even while the DAW/UI is
        // busy. This is a sleeping worker, not a realtime or busy-wait thread.
        if (!startThread(juce::Thread::Priority::high)) startThread();
    }
    else
    {
        enabled.store(false, std::memory_order_release);
    }
}

bool NetworkServer::startServer(const StreamConfig& newConfig)
{
    struct mg_callbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.log_message = &NetworkServer::logHandler;
    callbacks.log_access = &NetworkServer::logHandler;

    auto actualPort = newConfig.port;
    const auto networkSelection = selectBestLanInterface(actualPort, newConfig.usesHttps());
    CertificateFiles certificate;
    if (newConfig.usesHttps())
    {
        certificate = ensureSelfSignedCertificate(networkSelection.primaryAddress);
        if (! certificate.ready)
        {
            updateStatus("certificate error: " + certificate.description);
            contextRunning.store(false, std::memory_order_release);
            return false;
        }
    }

    auto portString = juce::String(actualPort) + (newConfig.usesHttps() ? "s" : "");
    const auto certificatePath = certificate.pemFile.getFullPathName();

    std::vector<const char*> options;
    options.reserve(24);
    options.push_back("listening_ports");
    options.push_back(portString.toRawUTF8());
    if (newConfig.usesHttps())
    {
        options.push_back("ssl_certificate");
        options.push_back(certificatePath.toRawUTF8());
    }
    options.push_back("num_threads");
    options.push_back("8");
    options.push_back("enable_websocket_ping_pong");
    options.push_back("yes");
    options.push_back("websocket_timeout_ms");
    options.push_back("30000");
    options.push_back("request_timeout_ms");
    options.push_back("1500");
    options.push_back("linger_timeout_ms");
    options.push_back("500");
    options.push_back("authentication_domain");
    options.push_back("MConnect");
    options.push_back(nullptr);

    for (int attempt = 0; attempt < 20 && actualPort <= 65535; ++attempt, ++actualPort)
    {
        portString = juce::String(actualPort) + (newConfig.usesHttps() ? "s" : "");
        options[1] = portString.toRawUTF8();
        context = mg_start(&callbacks, this, options.data());
        if (context) break;
    }
    if (context == nullptr)
    {
        updateStatus("port bind or server start failed");
        contextRunning.store(false, std::memory_order_release);
        return false;
    }

    mg_set_request_handler(context, "/", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/index.html", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/app.js", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/source.zip", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/license", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/style.css", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/healthz", &NetworkServer::requestHandler, this);
    mg_set_request_handler(context, "/info", &NetworkServer::requestHandler, this);
    mg_set_websocket_handler(context,
                             "/ws",
                             &NetworkServer::wsConnectHandler,
                             &NetworkServer::wsReadyHandler,
                             &NetworkServer::wsDataHandler,
                             &NetworkServer::wsCloseHandler,
                             this);

    listenPort.store(actualPort, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.port = selectedConfig.port = actualPort;
    }
    refreshLanSelection(actualPort, newConfig.usesHttps());
    updateStatus(newConfig.usesHttps() ? "streaming over HTTPS" : "streaming over HTTP");

    contextRunning.store(true, std::memory_order_release);
    return true;
}

void NetworkServer::stopServer()
{
    receiversActive.store(false, std::memory_order_release);
    enabled.store(false, std::memory_order_release);
    signalThreadShouldExit();
    wakeEvent->signal();
    if (isThreadRunning())
        stopThread(3000);

    webrtcSender.clear();
    if (context != nullptr)
    {
        mg_stop(context);
        context = nullptr;
    }

    webrtcSender.clear();
    contextRunning.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(websocketMutex);
        websocketConnections.clear();
    }

    std::lock_guard<std::mutex> lock(statusMutex);
    lanUrl.clear();
    currentLanIp.clear();
    candidateLanUrls.clear();
    candidateLanDescriptions.clear();
}

void NetworkServer::run()
{
    auto previousSourceRate = 0;
    auto previousWebRtcFrameFrames = size_t { 0 };
    auto lastSourceAudioMs = juce::Time::getMillisecondCounterHiRes();
    auto nextSilencePacketMs = lastSourceAudioMs;
    auto nextLanRefreshMs = lastSourceAudioMs;
    auto sourceHasBeenActive = false;
    auto starvationReported = false;

    StreamConfig localConfig;
    { std::lock_guard<std::mutex> lock(configMutex); localConfig = config; }
    const auto webRtcFrameFrames = juce::jmax<size_t> (1, webrtcSender.audioFrameCount());
    const auto webRtcFrameDurationMs = juce::jmax(1, static_cast<int>(std::round(
        static_cast<double>(webRtcFrameFrames) * 1000.0 / 48000.0)));
    while (!threadShouldExit())
    {
        workerWakeups.fetch_add(1, std::memory_order_relaxed);

        localConfig.sessionSampleRate = sessionSampleRate.load(std::memory_order_acquire);
        const auto sourceRate = juce::jmax(1, static_cast<int> (std::round(localConfig.sessionSampleRate)));
        const auto webRtcTracks = webrtcSender.openOutputCount();
        receiversActive.store(webRtcTracks > 0, std::memory_order_release);
        const auto nowMs = juce::Time::getMillisecondCounterHiRes();

        if (webRtcTracks == 0 && nowMs >= nextLanRefreshMs)
        {
            refreshLanSelection(localConfig.port, localConfig.usesHttps());
            nextLanRefreshMs = nowMs + 5000.0;
        }

        if (sourceRate != previousSourceRate || webRtcFrameFrames != previousWebRtcFrameFrames)
        {
            webrtcResampler.reset();
            fifo.discardFromConsumer(fifo.getAvailableFrames());
            webrtcSender.resetAudio();
            previousSourceRate = sourceRate;
            previousWebRtcFrameFrames = webRtcFrameFrames;
            lastSourceAudioMs = nowMs;
            nextSilencePacketMs = nowMs + webRtcFrameDurationMs;
            sourceHasBeenActive = false;
            starvationReported = false;
        }

        auto desiredReadFrames = sourceRate == 48000
            ? webRtcFrameFrames
            : static_cast<size_t> (std::ceil(static_cast<double> (webRtcFrameFrames) * static_cast<double> (sourceRate) / 48000.0));
        desiredReadFrames = juce::jlimit<size_t> (1, 4096, desiredReadFrames);
        if (readBuffer.size() < desiredReadFrames * 2)
            readBuffer.resize(desiredReadFrames * 2);

        if (webrtcSilenceBuffer.size() < webRtcFrameFrames * 2)
            webrtcSilenceBuffer.resize(webRtcFrameFrames * 2, 0.0f);

        if (webRtcTracks == 0)
        {
            webrtcSender.resetAudio();
            fifo.discardFromConsumer(fifo.getAvailableFrames());
            sourceHasBeenActive = false;
            starvationReported = false;
            lastSourceAudioMs = nowMs;
            wakeEvent->wait(500);
            continue;
        }

        // Consumer trims latency without racing the producer's cursor.
        const auto backlog = fifo.getAvailableFrames();
        const auto blockFrames = sourceBlockFrames.load(std::memory_order_acquire);
        const auto maximumBacklog = static_cast<size_t>(juce::jmax(sourceRate / 25, blockFrames * 2));
        if (backlog > maximumBacklog)
        {
            const auto skipped = backlog - maximumBacklog;
            fifo.discardFromConsumer(skipped);
            backlogDiscardedFrames.fetch_add(skipped, std::memory_order_relaxed);
            webrtcSender.skipAudio(static_cast<size_t>(std::llround(static_cast<double>(skipped)*48000.0/sourceRate)));
            webrtcResampler.reset();
        }
        const auto idleTimeoutMs = juce::jmax(static_cast<double>(webRtcFrameDurationMs * 2),
            1000.0 * sourceBlockFrames.load(std::memory_order_relaxed) / sourceRate + 10.0);
        // Batch SRC at one codec frame, using the source's clock. A stopped
        // source can still drain its final partial frame before silence.
        const bool waitForSrcFrame = sourceRate != 48000
            && fifo.getAvailableFrames() < desiredReadFrames
            && nowMs - lastSourceAudioMs < idleTimeoutMs;
        const auto framesRead = waitForSrcFrame ? size_t{0}
            : fifo.pop(readBuffer.data(), desiredReadFrames);
        if (framesRead > 0)
        {
            lastSourceAudioMs = nowMs;
            nextSilencePacketMs = nowMs + webRtcFrameDurationMs;
            sourceHasBeenActive = true;
            starvationReported = false;

            const float* webRtcFrames = readBuffer.data();
            auto webRtcFrameCount = framesRead;

            if (sourceRate != 48000)
            {
                try
                {
                    webrtcResampler.process(readBuffer.data(), framesRead, sourceRate, 48000, webrtcResampledBuffer);
                }
                catch (const std::exception&)
                {
                    conversionErrors.fetch_add(1, std::memory_order_relaxed);
                    webrtcResampler.reset();
                    webrtcResampledBuffer.clear();
                }
                webRtcFrames = webrtcResampledBuffer.data();
                webRtcFrameCount = webrtcResampledBuffer.size() / 2;
            }

            if (webRtcFrameCount > 0)
            {
                webrtcSender.encodeAndSend(webRtcFrames, webRtcFrameCount);
                framesSent.fetch_add(static_cast<uint64_t> (webRtcFrameCount), std::memory_order_relaxed);
            }

            juce::Thread::sleep(juce::jlimit(1, 10, webRtcFrameDurationMs / 4));
            continue;
        }

        if (nowMs - lastSourceAudioMs >= idleTimeoutMs)
        {
            if (sourceHasBeenActive && ! starvationReported)
            {
                serverFifoUnderruns.fetch_add(1, std::memory_order_relaxed);
                starvationReported = true;
            }

            if (!localConfig.keepAliveWhenIdle)
            {
                juce::Thread::sleep(10);
                continue;
            }

            if (nowMs >= nextSilencePacketMs)
            {
                webrtcSender.encodeAndSend(webrtcSilenceBuffer.data(), webRtcFrameFrames);
                framesSent.fetch_add(static_cast<uint64_t> (webRtcFrameFrames), std::memory_order_relaxed);
                nextSilencePacketMs = nowMs + webRtcFrameDurationMs;
            }

            juce::Thread::sleep(juce::jlimit(1, 20, webRtcFrameDurationMs / 2));
            continue;
        }

        juce::Thread::sleep(juce::jlimit(1, 10, webRtcFrameDurationMs / 4));
    }
}

bool NetworkServer::handleWebSocketText(mg_connection* connection, const char* data, size_t dataLen)
{
    if (!connection || !data || dataLen == 0 || dataLen > 65536) return false;
    if (!hasBoundedJsonNesting(data, dataLen)) return false;
    auto message = juce::JSON::parse(juce::String::fromUTF8(data, static_cast<int>(dataLen)));
    const auto* fields = message.getDynamicObject();
    if (!fields || !fields->getProperty("type").isString()) return false;
    const auto type = jsonPropertyString(message, "type");
    if (type == "webrtc-offer")
    {
        if (!fields->getProperty("sdp").isString() || fields->getProperty("sdp").toString().isEmpty()) return false;
        // A receiver cannot change the quality or transport of another listener.
        if (auto* object = message.getDynamicObject())
        {
            StreamConfig active;
            { std::lock_guard<std::mutex> lock(configMutex); active = config; }
            object->setProperty("transportMode", "opus");
            object->setProperty("bitrateBps", active.opusBitrateBps());
            object->setProperty("bitratePreset", active.opusBitrateName());
            object->setProperty("latencyPreset", active.latencyModeName());
        }
        webrtcSender.handleOffer(connection, message);
    }
    else if (type == "webrtc-candidate") webrtcSender.handleRemoteCandidate(connection, message);
    else if (type == "webrtc-ready")
    {
        if (!fields->getProperty("ready").isBool()) return false;
        webrtcSender.setReceiverReady(connection, static_cast<bool>(fields->getProperty("ready")));
    }
    else if (type == "webrtc-stop") webrtcSender.removeConnection(connection);
    else return false;
    return true;
}

void NetworkServer::noteStateChangeLocked(const char* origin, const char* reason)
{
    ++stateRevision;
    stateOrigin = origin;
    lastAdaptationReason = reason;
    lastAdaptationTimestamp = nowTimestamp();
}

void NetworkServer::refreshLanSelection(int port, bool useHttps)
{
    const auto networkSelection = selectBestLanInterface(port, useHttps);

    std::lock_guard<std::mutex> lock(statusMutex);
    lanUrl = networkSelection.primaryUrl;
    currentLanIp = networkSelection.primaryAddress;
    candidateLanUrls = networkSelection.candidateUrls;
    candidateLanDescriptions = networkSelection.candidateDescriptions;
    listenAddress = "0.0.0.0";
}

int NetworkServer::handleHttpRequest(mg_connection* connection)
{
    const auto* info = mg_get_request_info(connection);
    const auto* uri = info != nullptr ? info->local_uri : "/";

    if (std::strcmp(uri, "/healthz") == 0)
    {
        const char* json = "{\"ok\":true,\"name\":\"MConnect\"}";
        sendHttpOkHeaders(connection, "application/json; charset=utf-8", std::strlen(json));
        mg_write(connection, json, std::strlen(json));
        return 200;
    }

    if (std::strcmp(uri, "/info") == 0)
    {
        if (!isAuthorized(connection))
        {
            mg_send_http_error(connection, 403, "Pairing link required");
            return 403;
        }
        const auto stats = getStats();
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty("name", "MConnect");
        object->setProperty("version", MCONNECT_VERSION);
        object->setProperty("sampleRate", stats.inputSampleRate);
        object->setProperty("bitrate", stats.opusBitrateBps);
        object->setProperty("listeners", stats.webrtcOpenTracks);
        object->setProperty("frameMs", stats.opusFrameDurationMs);
        object->setProperty("senderQueueMs", stats.senderQueueFillMs);
        object->setProperty("sourceBlockMs", 1000.0 * sourceBlockFrames.load(std::memory_order_relaxed) / stats.inputSampleRate);
        object->setProperty("bufferMs", stats.activePlayoutDelayHintMs);
        object->setProperty("packetsSent", static_cast<juce::int64>(stats.webrtcRtpPacketsSent));
        object->setProperty("droppedFrames", static_cast<juce::int64>(stats.fifoDroppedFrames));
        object->setProperty("stereoPackets", static_cast<juce::int64>(stats.opusStereoPackets));
        object->setProperty("workerWakeups", static_cast<juce::int64>(workerWakeups.load(std::memory_order_relaxed)));
        object->setProperty("backlogDiscardedFrames", static_cast<juce::int64>(backlogDiscardedFrames.load(std::memory_order_relaxed)));
        object->setProperty("conversionErrors", static_cast<juce::int64>(conversionErrors.load(std::memory_order_relaxed)));
        object->setProperty("encodeErrors", static_cast<juce::int64>(stats.opusEncodeErrors));
        object->setProperty("connectionState", stats.webrtcConnectionState);
        object->setProperty("iceState", stats.webrtcIceConnectionState);
        object->setProperty("peakL", stats.inputRmsL);
        object->setProperty("peakR", stats.inputRmsR);
        const auto json = juce::JSON::toString(juce::var(object.release()));
        sendHttpOkHeaders(connection, "application/json; charset=utf-8", json.getNumBytesAsUTF8());
        mg_write(connection, json.toRawUTF8(), json.getNumBytesAsUTF8());
        return 200;
    }

    HtmlAsset asset;
    if (getHtmlAsset(uri, asset))
    {
        sendHttpOkHeaders(connection, asset.mimeType, asset.size);
        mg_write(connection, asset.data, asset.size);
        return 200;
    }

    mg_send_http_error(connection, 404, "Not found");
    return 404;
}

int NetworkServer::requestHandler(mg_connection* connection, void* cbdata)
{
    return static_cast<NetworkServer*> (cbdata)->handleHttpRequest(connection);
}

int NetworkServer::wsConnectHandler(const mg_connection* connection, void* cbdata)
{
    auto* server = static_cast<NetworkServer*>(cbdata);
    if (!server->isAuthorized(connection)) return 1;
    const auto* origin = mg_get_header(connection, "Origin");
    const auto* host = mg_get_header(connection, "Host");
    if (!host || (origin && juce::String(origin) != "http://" + juce::String(host))) return 1;
    std::lock_guard<std::mutex> lock(server->websocketMutex);
    if (server->websocketConnections.size() >= 4) return 1;
    server->websocketConnections.insert(const_cast<mg_connection*>(connection));
    return 0;
}

void NetworkServer::wsReadyHandler(mg_connection* connection, void* cbdata)
{
    auto* server = static_cast<NetworkServer*> (cbdata);
    {
        std::lock_guard<std::mutex> lock(server->websocketMutex);
        server->websocketConnections.insert(connection);
    }

}

int NetworkServer::wsDataHandler(mg_connection* connection, int bits, char* data, size_t dataLen, void* cbdata)
{
    if (dataLen > 65536) return 0;
    const auto opcode = bits & 0x0f;
    if (opcode == MG_WEBSOCKET_OPCODE_TEXT)
    {
        if (!(bits & 0x80)) return 0; // Bounded, complete JSON messages only.
        return static_cast<NetworkServer*>(cbdata)->handleWebSocketText(connection, data, dataLen) ? 1 : 0;
    }
    if (opcode == 0 || opcode == MG_WEBSOCKET_OPCODE_BINARY) return 0;

    return 1;
}

void NetworkServer::wsCloseHandler(const mg_connection* connection, void* cbdata)
{
    auto* server = static_cast<NetworkServer*> (cbdata);
    auto* mutableConnection = const_cast<mg_connection*> (connection);
    {
        std::lock_guard<std::mutex> lock(server->websocketMutex);
        server->websocketConnections.erase(mutableConnection);
    }
    server->webrtcSender.removeConnection(mutableConnection);
}

int NetworkServer::logHandler(const mg_connection*, const char*)
{
    return 1;
}

bool NetworkServer::isAuthorized(const mg_connection* connection) const
{
    const auto* request = mg_get_request_info(connection);
    char value[80] {};
    const char* header = mg_get_header(connection, "X-MConnect-Token");
    if (header)
        return pairingToken == juce::String(header);
    if (!request || !request->query_string) return false;
    const auto length = mg_get_var(request->query_string, std::strlen(request->query_string), "token", value, sizeof(value));
    return length == pairingToken.length() && pairingToken == juce::String(value);
}

juce::String NetworkServer::getPairingUrl(const juce::String& address) const
{
    std::lock_guard<std::mutex> lock(statusMutex);
    if (!contextRunning.load(std::memory_order_acquire) || lanUrl.isEmpty()) return {};
    auto base = lanUrl;
    for (const auto& candidate : candidateLanUrls)
        if (juce::URL(candidate).getDomain() == address) { base = candidate; break; }
    return base.trimCharactersAtEnd("/") + "/#" + pairingToken;
}

void NetworkServer::updateStatus(juce::String text)
{
    std::lock_guard<std::mutex> lock(statusMutex);
    statusText = std::move(text);
}

StreamStats NetworkServer::getStats() const
{
    StreamConfig localConfig;
    StreamConfig localSelectedConfig;
    uint64_t localStateRevision = 0;
    juce::String localStateOrigin;
    juce::String localLastAdaptationReason;
    juce::String localLastAdaptationTimestamp;
    juce::String localLastStateUpdateSentTimestamp;
    juce::String localAutonegotiationMode;
    juce::String localAutonegotiationState;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        localSelectedConfig = selectedConfig;
        localConfig = config;
        localStateRevision = stateRevision;
        localStateOrigin = stateOrigin;
        localLastAdaptationReason = lastAdaptationReason;
        localLastAdaptationTimestamp = lastAdaptationTimestamp;
        localLastStateUpdateSentTimestamp = lastStateUpdateSentTimestamp;
        localAutonegotiationMode = autonegotiationMode;
        localAutonegotiationState = autonegotiationState;
    }

    StreamStats stats;
    stats.serverRunning = contextRunning.load(std::memory_order_acquire);
    stats.streamEnabled = enabled.load(std::memory_order_acquire);
    stats.port = localConfig.port;
    stats.connectedClients = webrtcSender.peerCount();
    stats.transportMode = localConfig.transportModeName();
    stats.inputSampleRate = juce::jmax(1, static_cast<int>(std::round(sessionSampleRate.load(std::memory_order_acquire))));
    stats.selectedOpusBitrateBps = localSelectedConfig.opusBitrateBps();
    stats.selectedOpusBitratePreset = localSelectedConfig.opusBitrateName();
    stats.opusBitrateBps = localConfig.opusBitrateBps();
    stats.opusBitratePreset = localConfig.opusBitrateName();
    stats.opusBitrateLimited = false;
    stats.selectedLatencyMode = localSelectedConfig.latencyModeName();
    stats.latencyMode = localConfig.latencyModeName();
    stats.latencyTarget = localConfig.latencyTargetDescription();
    stats.opusFrameDurationMs = localConfig.opusFrameDurationMs();
    stats.activePlayoutDelayHintMs = localConfig.playoutDelayHintMs();
    stats.stateRevision = localStateRevision;
    stats.stateOrigin = localStateOrigin;
    stats.lastAdaptationReason = localLastAdaptationReason;
    stats.lastAdaptationTimestamp = localLastAdaptationTimestamp;
    stats.lastUserRequestedBitratePreset = localSelectedConfig.opusBitrateName();
    stats.lastUserRequestedLatencyMode = localSelectedConfig.latencyModeName();
    stats.lastStateUpdateSentTimestamp = localLastStateUpdateSentTimestamp;
    stats.autonegotiationMode = localAutonegotiationMode;
    stats.autonegotiationEnabled = localAutonegotiationState != "inactive";
    stats.autonegotiationState = localAutonegotiationState;
    stats.framesSent = framesSent.load(std::memory_order_relaxed);
    stats.serverFifoUnderruns = serverFifoUnderruns.load(std::memory_order_relaxed);
    stats.fifoDroppedFrames = fifo.getDroppedFrames();
    stats.senderQueueFillFrames = fifo.getAvailableFrames();
    stats.senderQueueCapacityFrames = fifo.getCapacityFrames();
    if (localConfig.sessionSampleRate > 1.0)
        stats.senderQueueFillMs = (static_cast<double> (stats.senderQueueFillFrames) / localConfig.sessionSampleRate) * 1000.0;

    webrtcSender.fillStats(stats);

    {
        std::lock_guard<std::mutex> lock(statusMutex);
        stats.lanUrl = lanUrl;
        stats.currentLanIp = currentLanIp;
        stats.listenAddress = listenAddress;
        stats.statusText = statusText;
        stats.candidateLanUrls = candidateLanUrls;
        stats.candidateLanDescriptions = candidateLanDescriptions;
    }

    return stats;
}
}
