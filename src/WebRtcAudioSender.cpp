// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "WebRtcAudioSender.h"
#include "NetworkInterfaceUtils.h"
#include <civetweb.h>
#include <opus.h>
#include <rtc/rtc.hpp>
#include <mbedtls/aes.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace mconnect
{
namespace
{
constexpr int opusSampleRate = 48000;
constexpr int pcmTransportSampleRate = opusSampleRate;
constexpr int opusChannels = 2;
constexpr int defaultOpusPayloadType = 111;
constexpr int maxRtpPayloadType = 127;
constexpr int defaultOpusComplexity = 5;
constexpr int fallbackOpusComplexity = 3;
constexpr double encodeBudgetRatio = 0.10;
constexpr size_t pcmChannels = 2;
constexpr size_t defaultPcmPacketFrames = 480;
constexpr size_t pcmHeaderBytes = 40;
constexpr size_t pcmBackpressureWarningBytes = 128u * 1024u;
constexpr size_t pcmBackpressureDangerBytes = 256u * 1024u;
constexpr size_t pcmBackpressurePanicBytes = 512u * 1024u;
constexpr uint32_t pcmFlagFirstPacket = 1u << 0u;
constexpr uint32_t pcmFlagFormatChanged = 1u << 1u;
constexpr uint32_t pcmFlagDiscontinuity = 1u << 2u;
constexpr uint32_t pcmFlagSenderDroppedOldAudio = 1u << 3u;
constexpr uint32_t pcmFlagClippingDetected = 1u << 4u;

juce::String jsonString(const juce::String& value)
{
    auto escaped = value.replace("\\", "\\\\")
                        .replace("\"", "\\\"")
                        .replace("\r", "\\r")
                        .replace("\n", "\\n");
    return "\"" + escaped + "\"";
}

juce::var property(const juce::var& object, const char* name)
{
    if (auto* dynamic = object.getDynamicObject())
        return dynamic->getProperty(name);

    return {};
}

juce::String propertyString(const juce::var& object, const char* name)
{
    return property(object, name).toString();
}

int propertyInt(const juce::var& object, const char* name, int fallback)
{
    const auto value = property(object, name);
    if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
        return static_cast<int> (value);

    const auto text = value.toString();
    return text.isNotEmpty() ? text.getIntValue() : fallback;
}

double propertyDouble(const juce::var& object, const char* name, double fallback)
{
    const auto value = property(object, name);
    if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
        return static_cast<double> (value);

    const auto text = value.toString();
    return text.isNotEmpty() ? text.getDoubleValue() : fallback;
}

bool propertyBool(const juce::var& object, const char* name, bool fallback)
{
    const auto value = property(object, name);
    if (value.isBool())
        return static_cast<bool> (value);
    if (value.isInt() || value.isInt64() || value.isDouble())
        return static_cast<int> (value) != 0;

    const auto text = value.toString().toLowerCase();
    if (text == "true" || text == "yes" || text == "1")
        return true;
    if (text == "false" || text == "no" || text == "0")
        return false;

    return fallback;
}

TransportMode parseTransportMode(const juce::var& message, TransportMode fallback)
{
    const auto value = propertyString(message, "transportMode").toLowerCase();

    if (value.contains("pcm16") || value.contains("pcm-16"))
        return TransportMode::pcm16DataChannel;
    if (value.contains("pcm24") || value.contains("pcm-24"))
        return TransportMode::pcm24DataChannel;
    if (value.contains("pcm32") || value.contains("pcm-32") || value.contains("float"))
        return TransportMode::pcm32FloatDataChannel;
    if (value.contains("auto"))
        return TransportMode::autoSelect;
    if (value.contains("opus"))
        return TransportMode::opusWebRtc;

    return fallback;
}

OpusBitrateMode parseBitrateMode(const juce::var& message, OpusBitrateMode fallback)
{
    const auto preset = propertyString(message, "bitratePreset").toLowerCase();
    const auto bps = propertyInt(message, "bitrateBps", 0);

    if (preset.contains("128") || bps == 128000)
        return OpusBitrateMode::goodPreview128;
    if (preset.contains("192") || bps == 192000)
        return OpusBitrateMode::veryGood192;
    if (preset.contains("256") || bps == 256000)
        return OpusBitrateMode::studioPreview256;
    if (preset.contains("510") || bps == 510000)
        return OpusBitrateMode::experimentalMax510;
    if (preset.contains("320") || bps == 320000)
        return OpusBitrateMode::highQuality320;

    return fallback;
}

LatencyMode parseLatencyMode(const juce::var& message, LatencyMode fallback)
{
    const auto preset = propertyString(message, "latencyPreset").toLowerCase();

    if (preset.contains("very"))
        return LatencyMode::verySafe;
    if (preset.contains("safe"))
        return LatencyMode::safe;
    if (preset.contains("ultra"))
        return LatencyMode::ultraLowExperimental;
    if (preset.contains("low"))
        return LatencyMode::lowLatency;
    if (preset.contains("medium") || preset.contains("balanced"))
        return LatencyMode::medium;

    return fallback;
}

uint32_t nextSsrc()
{
    static std::atomic<uint32_t> counter { 0x4f120000u };
    return counter.fetch_add(1, std::memory_order_relaxed) + 1u;
}

std::string opusFmtp(const StreamConfig& config)
{
    return "minptime=10;sprop-stereo=1;maxaveragebitrate="
        + std::to_string(config.opusBitrateBps())
        + ";useinbandfec=0;usedtx=0";
}

std::string toStdString(const juce::String& value)
{
    return value.toStdString();
}

void writeU16Le(std::vector<uint8_t>& data, size_t offset, uint16_t value)
{
    data[offset] = static_cast<uint8_t> (value & 0xffu);
    data[offset + 1] = static_cast<uint8_t> ((value >> 8u) & 0xffu);
}

void writeU32Le(std::vector<uint8_t>& data, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        data[offset + i] = static_cast<uint8_t> ((value >> (i * 8u)) & 0xffu);
}

void writeU64Le(std::vector<uint8_t>& data, size_t offset, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        data[offset + i] = static_cast<uint8_t> ((value >> (i * 8u)) & 0xffu);
}

int32_t floatToPcm24(float value)
{
    const auto clipped = juce::jlimit(-1.0f, 1.0f, value);
    return static_cast<int32_t> (std::lrint(static_cast<double> (clipped) * 8388607.0));
}

int16_t floatToPcm16(float value)
{
    const auto clipped = juce::jlimit(-1.0f, 1.0f, value);
    return static_cast<int16_t> (std::lrint(static_cast<double> (clipped) * 32767.0));
}

bool isClippedForIntegerPcm(float value)
{
    return value < -1.0f || value > 1.0f;
}

uint32_t float32ToBits(float value)
{
    const auto sanitized = std::isfinite(value) ? value : 0.0f;
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(sanitized), "float32 size mismatch");
    std::memcpy(&bits, &sanitized, sizeof(bits));
    return bits;
}

struct OfferAudioInfo
{
    juce::String mid { "audio" };
    int opusPayloadType = defaultOpusPayloadType;
};

OfferAudioInfo parseOfferAudioInfo(const juce::String& sdp)
{
    OfferAudioInfo result;
    auto inAudioSection = false;

    juce::StringArray lines;
    lines.addLines(sdp);

    for (auto line : lines)
    {
        line = line.trimCharactersAtEnd("\r").trim();
        if (line.startsWith("m="))
        {
            inAudioSection = line.startsWith("m=audio ");
            continue;
        }

        if (! inAudioSection)
            continue;

        if (line.startsWith("a=mid:"))
        {
            const auto mid = line.fromFirstOccurrenceOf("a=mid:", false, false).trim();
            if (mid.isNotEmpty())
                result.mid = mid;
            continue;
        }

        if (line.startsWith("a=rtpmap:") && line.containsIgnoreCase(" opus/48000"))
        {
            const auto payloadText = line.fromFirstOccurrenceOf("a=rtpmap:", false, false)
                                         .upToFirstOccurrenceOf(" ", false, false)
                                         .trim();
            const auto payloadType = payloadText.getIntValue();
            if (payloadType > 0 && payloadType <= maxRtpPayloadType)
                result.opusPayloadType = payloadType;
        }
    }

    return result;
}
}

struct WebRtcAudioSender::Peer
{
    mg_connection* connection = nullptr;
    std::mutex signalingMutex;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> dataChannel;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    TransportMode transportMode = TransportMode::opusWebRtc;
    std::atomic<bool> open { false };
    std::atomic<bool> receiverReady { false };
    std::atomic<bool> pcmOpen { false };
    std::atomic<bool> pcmReceiverReady { false };
    std::atomic<int> state { static_cast<int> (rtc::PeerConnection::State::New) };
    std::atomic<int> iceState { static_cast<int> (rtc::PeerConnection::IceState::New) };
};

WebRtcAudioSender::WebRtcAudioSender(std::shared_ptr<juce::WaitableEvent> activity)
    : activityEvent(std::move(activity)), encoder(nullptr, [] (OpusEncoder* encoderToDestroy)
      {
          if (encoderToDestroy != nullptr)
              opus_encoder_destroy(encoderToDestroy);
    })
{
    // Warm architecture-specific AES capability caches before any RTC worker
    // uses them. Older backends initialise these caches with volatile stores.
    static std::once_flag cryptoInitialized;
    std::call_once(cryptoInitialized, []
    {
        mbedtls_aes_context context;
        unsigned char key[16] {};
        mbedtls_aes_init(&context);
        static_cast<void>(mbedtls_aes_setkey_enc(&context, key, 128));
        mbedtls_aes_free(&context);
    });
    opusPacket.resize(4096);
}

WebRtcAudioSender::~WebRtcAudioSender()
{
    clear();
}

void WebRtcAudioSender::applyConfig(const StreamConfig& newConfig)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto previousTransportMode = config.transportMode;
    const auto previousLatencyMode = config.latencyMode;
    const auto previousPcmFrames = config.pcmPacketFrameCount(pcmTransportSampleRate);
    const auto previousPcmCodecId = config.pcmCodecId();
    config = newConfig;
    const auto pcmFrames = static_cast<size_t> (config.pcmPacketFrameCount(pcmTransportSampleRate));
    if (config.usesPcmDataChannel() && pcmPending.size() < pcmFrames * pcmChannels)
        pcmPending.resize(pcmFrames * pcmChannels);
    if (config.usesPcmDataChannel() && pcmPacket.size() < pcmHeaderBytes + pcmFrames * pcmChannels * 4)
        pcmPacket.resize(pcmHeaderBytes + pcmFrames * pcmChannels * 4);
    if (previousPcmFrames != config.pcmPacketFrameCount(pcmTransportSampleRate)
        || previousPcmCodecId != config.pcmCodecId()
        || previousTransportMode != config.transportMode
        || previousLatencyMode != config.latencyMode)
    {
        pcmPendingFrames = 0;
        pcmSampleCursor = 0;
        pcmSequence = 0;
        pcmDiscontinuityPending = false;
        pcmStreamId = pcmStreamId == std::numeric_limits<uint32_t>::max() ? 1u : pcmStreamId + 1u;
    }
    configureEncoderIfNeeded();
}

void WebRtcAudioSender::configureEncoderIfNeeded()
{
    const auto targetBitrate = config.opusBitrateBps();
    const auto targetFrameMs = config.opusFrameDurationMs();

    if (encoder != nullptr && encoderBitrateBps == targetBitrate && encoderFrameMs == targetFrameMs)
        return;

    recreateEncoder();
}

void WebRtcAudioSender::recreateEncoder()
{
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)> newEncoder(
        opus_encoder_create(opusSampleRate, opusChannels, OPUS_APPLICATION_AUDIO, &error),
        [] (OpusEncoder* encoderToDestroy)
        {
            if (encoderToDestroy != nullptr)
                opus_encoder_destroy(encoderToDestroy);
        });

    if (error != OPUS_OK || newEncoder == nullptr)
        return;

    const auto forceChannelsResult = opus_encoder_ctl(newEncoder.get(), OPUS_SET_FORCE_CHANNELS(opusChannels));
    if (forceChannelsResult != OPUS_OK)
    {
        opusForceChannelsErrors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto targetBitrate = config.opusBitrateBps();
    const auto targetFrameMs = config.opusFrameDurationMs();

    opus_encoder_ctl(newEncoder.get(), OPUS_SET_BITRATE(targetBitrate));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_DTX(0));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_PACKET_LOSS_PERC(0));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_COMPLEXITY(encoderComplexity));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_VBR(1));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(newEncoder.get(), OPUS_SET_LSB_DEPTH(24));

    encoder = std::move(newEncoder);
    encoderBitrateBps = targetBitrate;
    encoderFrameMs = targetFrameMs;
    encoderFrameFrames = static_cast<size_t> (opusSampleRate * targetFrameMs / 1000);
    encoderFrameFrames = juce::jmax<size_t> (120, encoderFrameFrames);
    pendingPcm.assign(encoderFrameFrames * opusChannels, 0.0f);
    pendingFrames = 0;
}

void WebRtcAudioSender::resetCountersLocked()
{
    encodedFrames.store(0, std::memory_order_relaxed);
    encodedPackets.store(0, std::memory_order_relaxed);
    encodedBytes.store(0, std::memory_order_relaxed);
    sendCalls.store(0, std::memory_order_relaxed);
    rtpPacketsAttempted.store(0, std::memory_order_relaxed);
    rtpPacketsSent.store(0, std::memory_order_relaxed);
    rtpSendFailures.store(0, std::memory_order_relaxed);
    encodeOverBudgetCount.store(0, std::memory_order_relaxed);
    timestampAnomalyCount.store(0, std::memory_order_relaxed);
    pcmOpenChannels.store(0, std::memory_order_relaxed);
    pcmReceiverReadyCount.store(0, std::memory_order_relaxed);
    pcmPacketsSent.store(0, std::memory_order_relaxed);
    pcmBytesSent.store(0, std::memory_order_relaxed);
    pcmSendCalls.store(0, std::memory_order_relaxed);
    pcmSendFailures.store(0, std::memory_order_relaxed);
    pcmDroppedBeforeSend.store(0, std::memory_order_relaxed);
    pcmReceiverStatsCount.store(0, std::memory_order_relaxed);
    opusEncodeErrors.store(0, std::memory_order_relaxed);
    opusForceChannelsErrors.store(0, std::memory_order_relaxed);
    opusMonoPackets.store(0, std::memory_order_relaxed);
    opusStereoPackets.store(0, std::memory_order_relaxed);
    opusPacketChannelErrors.store(0, std::memory_order_relaxed);
    opusLastEncodedChannels.store(0, std::memory_order_relaxed);
    opusPacketBytesTotal.store(0, std::memory_order_relaxed);
    opusPacketBytesLast.store(0, std::memory_order_relaxed);
    pcmPendingFrames = 0;
    pcmSampleCursor = 0;
    pcmSequence = 0;
    pcmDiscontinuityPending = false;
    pcmStreamId = pcmStreamId == std::numeric_limits<uint32_t>::max() ? 1u : pcmStreamId + 1u;
    lastReceiverReadyAckMs = 0.0;
    pcmReceiverBufferMs = 0.0;
    pcmReceiverUnderflows = 0;
    pcmReceiverOverflows = 0;
    pcmReceiverMissingPackets = 0;
    pcmReceiverLatePackets = 0;
    pcmAudioContextSampleRate = 0.0;
    pcmAudioContextBaseLatencyMs = -1.0;
    pcmAudioContextOutputLatencyMs = -1.0;
    pcmReceiverLastError.clear();
    haveLastSubmittedTimestamp = false;
    lastSubmittedTimestamp = 0;
    currentSequenceNumber = 0;
    currentTimestamp = 0;
    currentTimestampIncrementActual = 0;
    currentTimestampIncrementExpected = static_cast<uint32_t> (encoderFrameFrames);
    inputRmsL = 0.0;
    inputRmsR = 0.0;
    inputMidRms = 0.0;
    inputSideRms = 0.0;
    inputCorrelation = 0.0;
}

void WebRtcAudioSender::resetEncoderComplexityLocked()
{
    encoderComplexity = defaultOpusComplexity;
    if (encoder != nullptr)
        opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(encoderComplexity));
}

void WebRtcAudioSender::fallbackEncoderComplexityLocked()
{
    if (encoderComplexity <= fallbackOpusComplexity)
        return;

    if (encoder != nullptr)
    {
        const auto result = opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(fallbackOpusComplexity));
        if (result != OPUS_OK)
            return;
    }

    encoderComplexity = fallbackOpusComplexity;
}

void WebRtcAudioSender::handleOffer(mg_connection* connection, const juce::var& message)
{
    if (connection == nullptr)
        return;

    const auto sdp = propertyString(message, "sdp");
    if (sdp.isEmpty())
    {
        sendJson(connection, "{\"type\":\"webrtc-error\",\"message\":\"missing offer SDP\"}");
        return;
    }

    StreamConfig peerConfig;
    {
        std::lock_guard<std::mutex> lock(mutex);
        peerConfig = config;
        peerConfig.opusBitrateMode = parseBitrateMode(message, peerConfig.opusBitrateMode);
        peerConfig.latencyMode = parseLatencyMode(message, peerConfig.latencyMode);
        peerConfig.selectedTransportMode = parseTransportMode(message, peerConfig.selectedTransportMode);
        peerConfig.transportMode = StreamConfig::resolveTransportMode(peerConfig.selectedTransportMode,
                                                                       peerConfig.useSelfSignedCertificate);
        config.opusBitrateMode = peerConfig.opusBitrateMode;
        config.latencyMode = peerConfig.latencyMode;
        config.selectedTransportMode = peerConfig.selectedTransportMode;
        config.transportMode = peerConfig.transportMode;
        configureEncoderIfNeeded();
    }

    try
    {
        auto rtcConfig = rtc::Configuration();
        // We explicitly create the answer below; prevent an automatic second answer.
        rtcConfig.disableAutoNegotiation = true;
        rtcConfig.forceMediaTransport = ! peerConfig.usesPcmDataChannel();
        rtcConfig.enableIceTcp = false;
        // Use the LAN interface chosen for the QR link. Unrestricted ICE can
        // select a VPN interface and send local monitoring through its tunnel.
        const auto lan = selectBestLanInterface(peerConfig.port, peerConfig.usesHttps());
        rtcConfig.bindAddress = toStdString(lan.primaryAddress);

        auto peer = std::make_shared<Peer>();
        peer->connection = connection;
        peer->transportMode = peerConfig.transportMode;
        peer->pc = std::make_shared<rtc::PeerConnection>(rtcConfig);

        std::weak_ptr<Peer> weakPeer = peer;

        peer->pc->onLocalDescription([weakPeer] (rtc::Description description)
        {
            if (auto locked = weakPeer.lock())
            {
                std::lock_guard<std::mutex> signalingLock(locked->signalingMutex);
                if (!locked->connection) return;
                const auto json = juce::String("{\"type\":\"webrtc-answer\",\"sdpType\":")
                    + jsonString(description.typeString())
                    + ",\"sdp\":"
                    + jsonString(juce::String(std::string(description)))
                    + "}";
                sendJson(locked->connection, json);
            }
        });

        peer->pc->onLocalCandidate([weakPeer] (rtc::Candidate candidate)
        {
            if (auto locked = weakPeer.lock())
            {
                std::lock_guard<std::mutex> signalingLock(locked->signalingMutex);
                if (!locked->connection) return;
                const auto json = juce::String("{\"type\":\"webrtc-candidate\",\"candidate\":")
                    + jsonString(juce::String(candidate.candidate()))
                    + ",\"mid\":"
                    + jsonString(juce::String(candidate.mid()))
                    + "}";
                sendJson(locked->connection, json);
            }
        });

        peer->pc->onStateChange([weakPeer, wake = activityEvent] (rtc::PeerConnection::State state)
        {
            if (auto locked = weakPeer.lock())
            {
                locked->state.store(static_cast<int> (state), std::memory_order_release);
                if (state == rtc::PeerConnection::State::Connected && locked->track)
                    locked->open.store(locked->track->isOpen(), std::memory_order_release);
                if (state == rtc::PeerConnection::State::Failed
                    || state == rtc::PeerConnection::State::Closed)
                {
                    locked->open.store(false, std::memory_order_release);
                    locked->pcmOpen.store(false, std::memory_order_release);
                    locked->pcmReceiverReady.store(false, std::memory_order_release);
                }
                wake->signal();
            }
        });

        peer->pc->onIceStateChange([weakPeer] (rtc::PeerConnection::IceState state)
        {
            if (auto locked = weakPeer.lock())
                locked->iceState.store(static_cast<int> (state), std::memory_order_release);
        });

        OfferAudioInfo offerAudio;
        uint32_t ssrc = 0;

        if (peerConfig.usesPcmDataChannel())
        {
            peer->pc->onDataChannel([this, weakPeer] (std::shared_ptr<rtc::DataChannel> dataChannel)
            {
                if (auto locked = weakPeer.lock())
                    configureDataChannel(locked, std::move(dataChannel));
            });
        }
        else
        {
            offerAudio = parseOfferAudioInfo(sdp);
            ssrc = nextSsrc();
            const auto cname = std::string("mconnect-audio");
            auto audio = rtc::Description::Audio(toStdString(offerAudio.mid), rtc::Description::Direction::SendOnly);
            audio.addOpusCodec(offerAudio.opusPayloadType, opusFmtp(peerConfig));
            const auto ptime = std::to_string(peerConfig.opusFrameDurationMs());
            audio.addAttribute("ptime:" + ptime);
            audio.addAttribute("maxptime:" + ptime);
            audio.addSSRC(ssrc, cname, "mconnect", "mconnect-audio");

            peer->track = peer->pc->addTrack(audio);
            peer->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, cname, static_cast<uint8_t> (offerAudio.opusPayloadType), rtc::OpusRtpPacketizer::DefaultClockRate);

            auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(peer->rtpConfig);
            auto srReporter = std::make_shared<rtc::RtcpSrReporter>(peer->rtpConfig);
            packetizer->addToChain(srReporter);
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
            packetizer->addToChain(nackResponder);
            peer->track->setMediaHandler(packetizer);
            peer->track->onOpen([weakPeer, wake = activityEvent]
            {
                if (auto locked = weakPeer.lock())
                    locked->open.store(true, std::memory_order_release);
                wake->signal();
            });
            peer->track->onClosed([weakPeer, wake = activityEvent]
            {
                if (auto locked = weakPeer.lock())
                    locked->open.store(false, std::memory_order_release);
                wake->signal();
            });
        }

        PeerPtr oldPeer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = peers.find(connection);
            // Check under the insertion lock, including simultaneous offers.
            if (it == peers.end() && peers.size() >= 4)
                throw std::runtime_error("All four listener slots are in use");
            if (it != peers.end())
            {
                oldPeer = it->second;
                peers.erase(it);
            }

            if (peers.empty())
                resetCountersLocked();

            peers[connection] = peer;
            if (peerConfig.usesPcmDataChannel())
            {
                pcmPendingFrames = 0;
            }
            else
            {
                negotiatedPayloadType = offerAudio.opusPayloadType;
                currentSsrc = ssrc;
                currentSequenceNumber = peer->rtpConfig->sequenceNumber;
                currentTimestamp = peer->rtpConfig->timestamp;
                currentTimestampIncrementExpected = static_cast<uint32_t> (encoderFrameFrames);
            }
            haveLastSubmittedTimestamp = false;
        }

        closePeer(oldPeer);

        peer->pc->setRemoteDescription(rtc::Description(toStdString(sdp), "offer"));
        peer->pc->setLocalDescription(rtc::Description::Type::Answer);
    }
    catch (const std::exception& exception)
    {
        sendJson(connection,
                 juce::String("{\"type\":\"webrtc-error\",\"message\":")
                    + jsonString(exception.what())
                    + "}");
    }
}

void WebRtcAudioSender::handleRemoteCandidate(mg_connection* connection, const juce::var& message)
{
    auto peer = findPeer(connection);
    if (peer == nullptr || peer->pc == nullptr)
        return;

    const auto candidate = propertyString(message, "candidate");
    if (candidate.isEmpty())
        return;

    const auto mid = propertyString(message, "mid");

    try
    {
        peer->pc->addRemoteCandidate(rtc::Candidate(toStdString(candidate), toStdString(mid)));
    }
    catch (...)
    {
        sendJson(connection, "{\"type\":\"webrtc-error\",\"message\":\"failed to add ICE candidate\"}");
    }
}

void WebRtcAudioSender::setReceiverReady(mg_connection* connection, bool ready)
{
    if (auto peer = findPeer(connection))
        peer->receiverReady.store(ready, std::memory_order_release);
    activityEvent->signal();
}

void WebRtcAudioSender::configureDataChannel(PeerPtr peer, std::shared_ptr<rtc::DataChannel> dataChannel)
{
    if (peer == nullptr || dataChannel == nullptr)
        return;

    const auto label = juce::String(dataChannel->label());
    if (label.isNotEmpty() && label != "pgs-pcm-audio")
        return;

    {
        std::lock_guard<std::mutex> lock(mutex);
        peer->dataChannel = dataChannel;
    }

    std::weak_ptr<Peer> weakPeer = peer;
    dataChannel->onOpen([weakPeer]
    {
        if (auto locked = weakPeer.lock())
        {
            locked->pcmOpen.store(true, std::memory_order_release);
            locked->open.store(true, std::memory_order_release);
        }
    });

    dataChannel->onClosed([weakPeer]
    {
        if (auto locked = weakPeer.lock())
        {
            locked->pcmOpen.store(false, std::memory_order_release);
            locked->pcmReceiverReady.store(false, std::memory_order_release);
            locked->open.store(false, std::memory_order_release);
        }
    });

    dataChannel->onError([weakPeer] (std::string)
    {
        if (auto locked = weakPeer.lock())
        {
            locked->pcmOpen.store(false, std::memory_order_release);
            locked->pcmReceiverReady.store(false, std::memory_order_release);
            locked->open.store(false, std::memory_order_release);
        }
    });

    dataChannel->onMessage([weakPeer] (rtc::message_variant data)
    {
        if (auto locked = weakPeer.lock())
        {
            if (std::holds_alternative<std::string>(data)
                && juce::String(std::get<std::string>(data)).containsIgnoreCase("ready"))
                locked->pcmReceiverReady.store(true, std::memory_order_release);
        }
    });
}

void WebRtcAudioSender::updateReceiverStats(mg_connection* connection, const juce::var& message)
{
    const auto engine = propertyString(message, "currentEngine").toLowerCase();
    const auto ready = propertyBool(message, "dataChannelOpen", false)
        && propertyString(message, "audioContextState").toLowerCase() == "running"
        && propertyBool(message, "audioWorkletAvailable", false)
        && propertyBool(message, "audioWorkletModuleLoaded", false)
        && propertyBool(message, "audioWorkletNodeCreated", false)
        && propertyBool(message, "audioWorkletProcessorReady", false)
        && propertyBool(message, "ringBufferReady", false)
        && propertyBool(message, "secureContextOk", false)
        && engine.contains("pcm");

    std::lock_guard<std::mutex> lock(mutex);
    const auto it = peers.find(connection);
    if (it == peers.end() || it->second == nullptr)
        return;

    auto& peer = it->second;
    const auto wasReady = peer->pcmReceiverReady.load(std::memory_order_acquire);
    peer->pcmReceiverReady.store(ready, std::memory_order_release);

    if (ready && ! wasReady)
    {
        pcmPendingFrames = 0;
        pcmSampleCursor = 0;
        pcmSequence = 0;
        pcmDiscontinuityPending = false;
        pcmStreamId = pcmStreamId == std::numeric_limits<uint32_t>::max() ? 1u : pcmStreamId + 1u;
    }

    lastReceiverReadyAckMs = juce::Time::getMillisecondCounterHiRes();
    pcmReceiverBufferMs = propertyDouble(message, "bufferMs", pcmReceiverBufferMs);
    pcmReceiverUnderflows = static_cast<uint64_t> (juce::jmax(0, propertyInt(message, "underflows", static_cast<int> (pcmReceiverUnderflows))));
    pcmReceiverOverflows = static_cast<uint64_t> (juce::jmax(0, propertyInt(message, "overflows", static_cast<int> (pcmReceiverOverflows))));
    pcmReceiverMissingPackets = static_cast<uint64_t> (juce::jmax(0, propertyInt(message, "missingPackets", static_cast<int> (pcmReceiverMissingPackets))));
    pcmReceiverLatePackets = static_cast<uint64_t> (juce::jmax(0, propertyInt(message, "latePackets", static_cast<int> (pcmReceiverLatePackets))));
    pcmAudioContextSampleRate = propertyDouble(message, "audioContextSampleRate", pcmAudioContextSampleRate);
    pcmAudioContextBaseLatencyMs = propertyDouble(message, "audioContextBaseLatencyMs", pcmAudioContextBaseLatencyMs);
    pcmAudioContextOutputLatencyMs = propertyDouble(message, "audioContextOutputLatencyMs", pcmAudioContextOutputLatencyMs);
    pcmReceiverLastError = propertyString(message, "lastError");
    pcmReceiverStatsCount.fetch_add(1, std::memory_order_relaxed);
}

void WebRtcAudioSender::removeConnection(mg_connection* connection)
{
    PeerPtr peer;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = peers.find(connection);
        if (it == peers.end())
            return;

        peer = it->second;
        peers.erase(it);
        if (peers.empty())
        {
            pendingFrames = 0;
            encodedSampleCursor = 0;
            resetCountersLocked();
            resetEncoderComplexityLocked();
        }
    }

    closePeer(peer);
}

void WebRtcAudioSender::clear()
{
    std::vector<PeerPtr> toClose;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& entry : peers)
            toClose.push_back(entry.second);
        peers.clear();
        pendingFrames = 0;
        encodedSampleCursor = 0;
        resetCountersLocked();
        resetEncoderComplexityLocked();
    }

    for (auto& peer : toClose)
        closePeer(peer);
}

void WebRtcAudioSender::closePeer(PeerPtr peer)
{
    if (peer == nullptr)
        return;

    {
        std::lock_guard<std::mutex> signalingLock(peer->signalingMutex);
        peer->connection = nullptr;
    }
    peer->open.store(false, std::memory_order_release);
    peer->receiverReady.store(false, std::memory_order_release);
    peer->pcmOpen.store(false, std::memory_order_release);
    peer->pcmReceiverReady.store(false, std::memory_order_release);

    if (peer->track != nullptr)
        peer->track->close();

    if (peer->dataChannel != nullptr)
        peer->dataChannel->close();

    if (peer->pc != nullptr)
    {
        peer->pc->resetCallbacks();
        peer->pc->close();
    }
}

WebRtcAudioSender::PeerPtr WebRtcAudioSender::findPeer(mg_connection* connection) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = peers.find(connection);
    return it != peers.end() ? it->second : nullptr;
}

void WebRtcAudioSender::encodeAndSend(const float* interleavedStereo48k, size_t frameCount)
{
    if (interleavedStereo48k == nullptr || frameCount == 0)
        return;

    std::lock_guard<std::mutex> lock(mutex);

    if (config.usesPcmDataChannel())
    {
        const auto packetFrames = static_cast<size_t> (config.pcmPacketFrameCount(pcmTransportSampleRate));
        if (pcmPending.size() < packetFrames * pcmChannels)
            pcmPending.resize(packetFrames * pcmChannels);
        if (pcmPacket.size() < pcmHeaderBytes + packetFrames * pcmChannels * 4)
            pcmPacket.resize(pcmHeaderBytes + packetFrames * pcmChannels * 4);

        const auto anyOpen = std::any_of(peers.begin(), peers.end(), [] (const auto& entry)
        {
            return entry.second != nullptr
                && entry.second->pcmOpen.load(std::memory_order_acquire);
        });

        if (! anyOpen)
        {
            pcmPendingFrames = 0;
            return;
        }

        size_t sourceFrame = 0;
        while (sourceFrame < frameCount)
        {
            const auto writable = std::min(packetFrames - pcmPendingFrames, frameCount - sourceFrame);
            std::copy(interleavedStereo48k + sourceFrame * pcmChannels,
                      interleavedStereo48k + (sourceFrame + writable) * pcmChannels,
                      pcmPending.data() + pcmPendingFrames * pcmChannels);

            pcmPendingFrames += writable;
            sourceFrame += writable;

            if (pcmPendingFrames < packetFrames)
                continue;

            sendPcmFrameLocked(pcmPending.data(), packetFrames);
            pcmPendingFrames = 0;
        }

        return;
    }

    const auto anyOpen = std::any_of(peers.begin(), peers.end(), [] (const auto& entry)
    {
        return entry.second != nullptr && entry.second->open.load(std::memory_order_acquire)
            && entry.second->receiverReady.load(std::memory_order_acquire);
    });

    if (! anyOpen)
    {
        pendingFrames = 0;
        return;
    }

    configureEncoderIfNeeded();

    if (encoder == nullptr || encoderFrameFrames == 0)
        return;

    size_t sourceFrame = 0;
    while (sourceFrame < frameCount)
    {
        const auto writable = std::min(encoderFrameFrames - pendingFrames, frameCount - sourceFrame);
        std::copy(interleavedStereo48k + sourceFrame * opusChannels,
                  interleavedStereo48k + (sourceFrame + writable) * opusChannels,
                  pendingPcm.data() + pendingFrames * opusChannels);

        pendingFrames += writable;
        sourceFrame += writable;

        if (pendingFrames < encoderFrameFrames)
            continue;

        const auto encodeStarted = juce::Time::getMillisecondCounterHiRes();
        if (++meterFrameCounter >= 5)
        {
            meterFrameCounter = 0;
            double squaresL = 0.0, squaresR = 0.0;
            for (size_t i = 0; i < encoderFrameFrames; ++i)
            {
                const double l = pendingPcm[i * 2], r = pendingPcm[i * 2 + 1];
                squaresL += l * l; squaresR += r * r;
            }
            inputRmsL = std::sqrt(squaresL / static_cast<double>(encoderFrameFrames));
            inputRmsR = std::sqrt(squaresR / static_cast<double>(encoderFrameFrames));
        }

        const auto encoded = opus_encode_float(encoder.get(),
                                               pendingPcm.data(),
                                               static_cast<int> (encoderFrameFrames),
                                               opusPacket.data(),
                                               static_cast<opus_int32> (opusPacket.size()));
        const auto encodeMs = juce::Time::getMillisecondCounterHiRes() - encodeStarted;

        if (encodeMs > static_cast<double> (encoderFrameMs) * encodeBudgetRatio)
        {
            encodeOverBudgetCount.fetch_add(1, std::memory_order_relaxed);
            fallbackEncoderComplexityLocked();
        }

        if (encoded > 0)
        {
            const auto encodedChannels = opus_packet_get_nb_channels(opusPacket.data());
            opusLastEncodedChannels.store(encodedChannels == 1 || encodedChannels == 2 ? encodedChannels : 0,
                                          std::memory_order_relaxed);
            if (encodedChannels == 2)
                opusStereoPackets.fetch_add(1, std::memory_order_relaxed);
            else if (encodedChannels == 1)
                opusMonoPackets.fetch_add(1, std::memory_order_relaxed);
            else
                opusPacketChannelErrors.fetch_add(1, std::memory_order_relaxed);

            opusPacketBytesLast.store(encoded, std::memory_order_relaxed);
            opusPacketBytesTotal.fetch_add(static_cast<uint64_t> (encoded), std::memory_order_relaxed);
            const auto sent = sendEncodedFrameLocked(opusPacket.data(),
                                                     static_cast<size_t> (encoded),
                                                     encodedSampleCursor);
            if (sent.sent > 0)
            {
                encodedFrames.fetch_add(encoderFrameFrames, std::memory_order_relaxed);
                encodedPackets.fetch_add(1, std::memory_order_relaxed);
                encodedBytes.fetch_add(static_cast<uint64_t> (encoded), std::memory_order_relaxed);
            }
        }
        else
        {
            opusEncodeErrors.fetch_add(1, std::memory_order_relaxed);
        }

        // A failed send/encode still consumes source time. Reusing its RTP
        // timestamp would compress the timeline when delivery recovers.
        encodedSampleCursor += encoderFrameFrames;
        pendingFrames = 0;
    }
}

WebRtcAudioSender::SendResult WebRtcAudioSender::sendEncodedFrameLocked(const uint8_t* data, size_t bytes, uint64_t frameStartSample)
{
    SendResult result;
    if (data == nullptr || bytes == 0)
        return result;

    auto* byteData = reinterpret_cast<const rtc::byte*> (data);

    for (const auto& entry : peers)
    {
        const auto& peer = entry.second;
        if (peer == nullptr || peer->track == nullptr || ! peer->open.load(std::memory_order_acquire)
            || !peer->receiverReady.load(std::memory_order_acquire))
            continue;

        ++result.attempted;
        try
        {
            const auto timestamp = peer->rtpConfig->startTimestamp + static_cast<uint32_t>(frameStartSample);
            peer->track->sendFrame(byteData, bytes, rtc::FrameInfo(timestamp));
            sendCalls.fetch_add(1, std::memory_order_relaxed);
            ++result.sent;
            if (peer->rtpConfig != nullptr)
            {
                currentSequenceNumber = static_cast<uint16_t> (peer->rtpConfig->sequenceNumber - 1u);
                currentTimestamp = peer->rtpConfig->timestamp;
                currentTimestampIncrementExpected = static_cast<uint32_t> (encoderFrameFrames);
            }
        }
        catch (...)
        {
            peer->open.store(false, std::memory_order_release);
            ++result.failed;
        }
    }

    if (result.attempted > 0)
        rtpPacketsAttempted.fetch_add(result.attempted, std::memory_order_relaxed);
    if (result.sent > 0)
        rtpPacketsSent.fetch_add(result.sent, std::memory_order_relaxed);
    if (result.failed > 0)
        rtpSendFailures.fetch_add(result.failed, std::memory_order_relaxed);

    if (result.sent > 0)
    {
        if (haveLastSubmittedTimestamp)
        {
            currentTimestampIncrementActual = currentTimestamp - lastSubmittedTimestamp;
            if (currentTimestampIncrementActual != currentTimestampIncrementExpected)
                timestampAnomalyCount.fetch_add(1, std::memory_order_relaxed);
        }

        lastSubmittedTimestamp = currentTimestamp;
        haveLastSubmittedTimestamp = true;
    }

    return result;
}

WebRtcAudioSender::SendResult WebRtcAudioSender::sendPcmFrameLocked(const float* interleavedStereo48k, size_t frameCount)
{
    SendResult result;

    if (interleavedStereo48k == nullptr || frameCount == 0)
        return result;

    const auto codecId = config.pcmCodecId();
    const auto bytesPerSample = codecId == 1 ? size_t { 2 } : (codecId == 2 ? size_t { 3 } : size_t { 4 });
    const auto payloadBytes = frameCount * pcmChannels * bytesPerSample;
    pcmPacket.assign(pcmHeaderBytes + payloadBytes, 0);
    pcmPacket[0] = 'P';
    pcmPacket[1] = 'G';
    pcmPacket[2] = 'P';
    pcmPacket[3] = 'C';
    pcmPacket[4] = 1;
    pcmPacket[5] = static_cast<uint8_t> (codecId);
    pcmPacket[6] = static_cast<uint8_t> (pcmChannels);
    pcmPacket[7] = static_cast<uint8_t> (pcmHeaderBytes);
    writeU32Le(pcmPacket, 8, pcmTransportSampleRate);
    writeU32Le(pcmPacket, 12, static_cast<uint32_t> (pcmSequence & 0xffffffffu));
    writeU32Le(pcmPacket, 16, static_cast<uint32_t> (pcmSampleCursor & 0xffffffffu));
    writeU32Le(pcmPacket, 20, static_cast<uint32_t> ((pcmSampleCursor >> 32u) & 0xffffffffu));
    writeU32Le(pcmPacket, 24, static_cast<uint32_t> (frameCount));
    writeU32Le(pcmPacket, 28, static_cast<uint32_t> (payloadBytes));
    writeU32Le(pcmPacket, 32, pcmStreamId);

    double sumSquaresL = 0.0;
    double sumSquaresR = 0.0;
    auto flags = uint32_t { 0 };
    if (pcmSequence == 0)
        flags |= pcmFlagFirstPacket | pcmFlagFormatChanged;
    if (pcmDiscontinuityPending)
        flags |= pcmFlagSenderDroppedOldAudio | pcmFlagDiscontinuity;
    auto offset = pcmHeaderBytes;
    for (size_t i = 0; i < frameCount; ++i)
    {
        const auto left = interleavedStereo48k[i * pcmChannels];
        const auto right = interleavedStereo48k[i * pcmChannels + 1];
        sumSquaresL += static_cast<double> (left) * static_cast<double> (left);
        sumSquaresR += static_cast<double> (right) * static_cast<double> (right);

        if (codecId != 3 && (isClippedForIntegerPcm(left) || isClippedForIntegerPcm(right)))
            flags |= pcmFlagClippingDetected;

        if (codecId == 1)
        {
            const auto left16 = static_cast<uint16_t> (floatToPcm16(left));
            const auto right16 = static_cast<uint16_t> (floatToPcm16(right));
            writeU16Le(pcmPacket, offset, left16);
            offset += 2;
            writeU16Le(pcmPacket, offset, right16);
            offset += 2;
        }
        else if (codecId == 2)
        {
            const auto left24 = static_cast<uint32_t> (floatToPcm24(left));
            const auto right24 = static_cast<uint32_t> (floatToPcm24(right));
            pcmPacket[offset++] = static_cast<uint8_t> (left24 & 0xffu);
            pcmPacket[offset++] = static_cast<uint8_t> ((left24 >> 8u) & 0xffu);
            pcmPacket[offset++] = static_cast<uint8_t> ((left24 >> 16u) & 0xffu);
            pcmPacket[offset++] = static_cast<uint8_t> (right24 & 0xffu);
            pcmPacket[offset++] = static_cast<uint8_t> ((right24 >> 8u) & 0xffu);
            pcmPacket[offset++] = static_cast<uint8_t> ((right24 >> 16u) & 0xffu);
        }
        else
        {
            writeU32Le(pcmPacket, offset, float32ToBits(left));
            offset += 4;
            writeU32Le(pcmPacket, offset, float32ToBits(right));
            offset += 4;
        }
    }

    inputRmsL = std::sqrt(sumSquaresL / static_cast<double> (frameCount));
    inputRmsR = std::sqrt(sumSquaresR / static_cast<double> (frameCount));
    writeU32Le(pcmPacket, 36, flags);

    auto droppedForBackpressure = false;

    for (const auto& entry : peers)
    {
        const auto& peer = entry.second;
        if (peer == nullptr
            || peer->dataChannel == nullptr
            || ! peer->pcmOpen.load(std::memory_order_acquire))
            continue;

        if (! peer->pcmReceiverReady.load(std::memory_order_acquire))
            continue;

        ++result.attempted;
        try
        {
            const auto buffered = peer->dataChannel->bufferedAmount();
            juce::ignoreUnused(pcmBackpressureWarningBytes, pcmBackpressurePanicBytes);
            if (buffered >= pcmBackpressureDangerBytes)
            {
                droppedForBackpressure = true;
                ++result.failed;
                continue;
            }

            peer->dataChannel->send(reinterpret_cast<const rtc::byte*> (pcmPacket.data()), pcmPacket.size());
            ++result.sent;
        }
        catch (...)
        {
            peer->pcmOpen.store(false, std::memory_order_release);
            peer->open.store(false, std::memory_order_release);
            ++result.failed;
        }
    }

    if (result.sent > 0)
    {
        pcmSendCalls.fetch_add(result.sent, std::memory_order_relaxed);
        pcmPacketsSent.fetch_add(result.sent, std::memory_order_relaxed);
        pcmBytesSent.fetch_add(static_cast<uint64_t> (pcmPacket.size() * result.sent), std::memory_order_relaxed);
        pcmDiscontinuityPending = false;
    }

    if (result.attempted > 0)
    {
        pcmSampleCursor += frameCount;
        ++pcmSequence;
    }

    if (droppedForBackpressure)
    {
        pcmDroppedBeforeSend.fetch_add(1, std::memory_order_relaxed);
    }

    if (result.failed > 0)
    {
        pcmSendFailures.fetch_add(result.failed, std::memory_order_relaxed);
        pcmDiscontinuityPending = true;
    }

    return result;
}

void WebRtcAudioSender::resetAudio()
{
    std::lock_guard<std::mutex> lock(mutex);
    pendingFrames = 0;
    pcmPendingFrames = 0;
    if (peers.empty()) encodedSampleCursor = 0;
    pcmSampleCursor = 0;
    pcmSequence = 0;
    pcmDiscontinuityPending = false;
    haveLastSubmittedTimestamp = false;
    currentTimestampIncrementActual = 0;
}

void WebRtcAudioSender::skipAudio(size_t skippedFrames48k)
{
    std::lock_guard<std::mutex> lock(mutex);
    encodedSampleCursor += skippedFrames48k + pendingFrames;
    pendingFrames = 0;
    haveLastSubmittedTimestamp = false;
}

int WebRtcAudioSender::peerCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<int> (peers.size());
}

int WebRtcAudioSender::openTrackCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<int> (std::count_if(peers.begin(), peers.end(), [] (const auto& entry)
    {
        return entry.second != nullptr && entry.second->open.load(std::memory_order_acquire)
            && entry.second->receiverReady.load(std::memory_order_acquire);
    }));
}

int WebRtcAudioSender::openOutputCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (config.usesPcmDataChannel())
    {
        return static_cast<int> (std::count_if(peers.begin(), peers.end(), [] (const auto& entry)
        {
            return entry.second != nullptr
                && entry.second->pcmOpen.load(std::memory_order_acquire);
        }));
    }

    return static_cast<int> (std::count_if(peers.begin(), peers.end(), [] (const auto& entry)
    {
        return entry.second != nullptr
            && entry.second->open.load(std::memory_order_acquire)
            && entry.second->receiverReady.load(std::memory_order_acquire);
    }));
}

bool WebRtcAudioSender::hasOpenTrack() const
{
    return openOutputCount() > 0;
}

size_t WebRtcAudioSender::opusFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return encoderFrameFrames;
}

size_t WebRtcAudioSender::audioFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return config.usesPcmDataChannel() ? static_cast<size_t> (config.pcmPacketFrameCount(pcmTransportSampleRate)) : encoderFrameFrames;
}

void WebRtcAudioSender::fillStats(StreamStats& stats) const
{
    stats.webrtcEncodedFrames = encodedFrames.load(std::memory_order_relaxed);
    stats.webrtcEncodedPackets = encodedPackets.load(std::memory_order_relaxed);
    stats.webrtcEncodedBytes = encodedBytes.load(std::memory_order_relaxed);
    stats.webrtcSendCalls = sendCalls.load(std::memory_order_relaxed);
    stats.webrtcRtpPacketsAttempted = rtpPacketsAttempted.load(std::memory_order_relaxed);
    stats.webrtcRtpPacketsSent = rtpPacketsSent.load(std::memory_order_relaxed);
    stats.webrtcRtpSendFailures = rtpSendFailures.load(std::memory_order_relaxed);
    stats.webrtcEncodeOverBudgetCount = encodeOverBudgetCount.load(std::memory_order_relaxed);
    stats.webrtcEncoderOverloadWarnings = stats.webrtcEncodeOverBudgetCount;
    stats.webrtcTimestampAnomalyCount = timestampAnomalyCount.load(std::memory_order_relaxed);
    stats.webrtcPacketsSubmittedToTrack = stats.webrtcRtpPacketsSent;
    stats.webrtcBytesSubmittedToTrack = encodedBytes.load(std::memory_order_relaxed);
    stats.webrtcSubmitErrors = stats.webrtcRtpSendFailures;
    stats.pcmPacketsSent = pcmPacketsSent.load(std::memory_order_relaxed);
    stats.pcmBytesSent = pcmBytesSent.load(std::memory_order_relaxed);
    stats.pcmSendCalls = pcmSendCalls.load(std::memory_order_relaxed);
    stats.pcmSendFailures = pcmSendFailures.load(std::memory_order_relaxed);
    stats.pcmDroppedBeforeSend = pcmDroppedBeforeSend.load(std::memory_order_relaxed);
    stats.pcmReceiverStatsCount = pcmReceiverStatsCount.load(std::memory_order_relaxed);
    stats.opusEncodeErrors = opusEncodeErrors.load(std::memory_order_relaxed);
    stats.opusForceChannelsErrors = opusForceChannelsErrors.load(std::memory_order_relaxed);
    stats.opusMonoPackets = opusMonoPackets.load(std::memory_order_relaxed);
    stats.opusStereoPackets = opusStereoPackets.load(std::memory_order_relaxed);
    stats.opusPacketChannelErrors = opusPacketChannelErrors.load(std::memory_order_relaxed);
    stats.opusLastEncodedChannels = opusLastEncodedChannels.load(std::memory_order_relaxed);
    stats.opusPacketBytesLast = opusPacketBytesLast.load(std::memory_order_relaxed);
    const auto packetCount = encodedPackets.load(std::memory_order_relaxed);
    stats.opusPacketBytesAvg = packetCount > 0
        ? static_cast<double> (opusPacketBytesTotal.load(std::memory_order_relaxed)) / static_cast<double> (packetCount)
        : 0.0;

    PeerPtr newestPeer;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stats.opusBitrateBps = config.opusBitrateBps();
        stats.opusBitratePreset = config.opusBitrateName();
        stats.opusBitrateLimited = false;
        stats.latencyMode = config.latencyModeName();
        stats.latencyTarget = config.latencyTargetDescription();
        stats.opusFrameDurationMs = config.opusFrameDurationMs();
        stats.transportMode = config.transportModeName();
        stats.selectedTransportMode = config.selectedTransportModeName();
        stats.httpsEnabled = config.usesHttps();
        stats.selfSignedCertificateEnabled = config.useSelfSignedCertificate;
        stats.audioPassthrough = config.audioPassthrough;
        stats.serverScheme = config.serverScheme();
        stats.pcmBitsPerSample = config.usesPcmDataChannel() ? config.pcmBitsPerSample() : 0;
        stats.pcmPacketFrames = config.pcmPacketFrameCount(pcmTransportSampleRate);
        stats.pcmPacketDurationMs = config.pcmPacketDurationMs();
        stats.pcmTargetBufferMs = config.pcmTargetBufferMs();
        stats.pcmResumeBufferMs = config.pcmResumeBufferMs();
        stats.pcmRingCapacityMs = config.pcmRingCapacityMs();
        stats.webrtcPeerCount = static_cast<int> (peers.size());
        stats.webrtcOpenTracks = static_cast<int> (std::count_if(peers.begin(), peers.end(), [] (const auto& entry)
        {
            return entry.second != nullptr && entry.second->open.load(std::memory_order_acquire)
            && entry.second->receiverReady.load(std::memory_order_acquire);
        }));
        stats.pcmOpenChannels = static_cast<int> (std::count_if(peers.begin(), peers.end(), [] (const auto& entry)
        {
            return entry.second != nullptr && entry.second->pcmOpen.load(std::memory_order_acquire);
        }));
        uint64_t maxBufferedAmount = 0;
        for (const auto& entry : peers)
        {
            if (entry.second != nullptr && entry.second->dataChannel != nullptr)
                maxBufferedAmount = std::max<uint64_t>(maxBufferedAmount,
                                                       static_cast<uint64_t> (entry.second->dataChannel->bufferedAmount()));
        }
        stats.pcmDataChannelBufferedBytes = maxBufferedAmount;
        stats.pcmReceiverReadyCount = static_cast<int> (std::count_if(peers.begin(), peers.end(), [] (const auto& entry)
        {
            return entry.second != nullptr && entry.second->pcmReceiverReady.load(std::memory_order_acquire);
        }));
        stats.pcmSequenceCurrent = pcmSequence;
        stats.pcmSampleCursor = pcmSampleCursor;
        stats.pcmReceiverBufferMs = pcmReceiverBufferMs;
        stats.pcmReceiverAckAgeMs = lastReceiverReadyAckMs > 0.0
            ? juce::Time::getMillisecondCounterHiRes() - lastReceiverReadyAckMs
            : -1.0;
        stats.pcmReceiverUnderflows = pcmReceiverUnderflows;
        stats.pcmReceiverOverflows = pcmReceiverOverflows;
        stats.pcmReceiverMissingPackets = pcmReceiverMissingPackets;
        stats.pcmReceiverLatePackets = pcmReceiverLatePackets;
        stats.pcmAudioContextSampleRate = pcmAudioContextSampleRate;
        stats.pcmAudioContextBaseLatencyMs = pcmAudioContextBaseLatencyMs;
        stats.pcmAudioContextOutputLatencyMs = pcmAudioContextOutputLatencyMs;
        stats.pcmReceiverLastError = pcmReceiverLastError;
        stats.webrtcNegotiatedPayloadType = negotiatedPayloadType;
        stats.webrtcActualPayloadType = negotiatedPayloadType;
        stats.webrtcSsrc = currentSsrc;
        stats.webrtcSequenceCurrent = currentSequenceNumber;
        stats.webrtcTimestampCurrent = currentTimestamp;
        stats.webrtcTimestampIncrementExpected = currentTimestampIncrementExpected;
        stats.webrtcTimestampIncrementActual = currentTimestampIncrementActual;
        stats.inputRmsL = inputRmsL;
        stats.inputRmsR = inputRmsR;
        stats.inputMidRms = inputMidRms;
        stats.inputSideRms = inputSideRms;
        stats.inputCorrelation = inputCorrelation;

        if (! peers.empty())
            newestPeer = peers.begin()->second;
    }

    if (newestPeer != nullptr)
    {
        stats.webrtcConnectionState = stateName(newestPeer->state.load(std::memory_order_acquire));
        stats.webrtcIceConnectionState = iceStateName(newestPeer->iceState.load(std::memory_order_acquire));
    }
    else
    {
        stats.webrtcConnectionState = "closed";
        stats.webrtcIceConnectionState = "closed";
    }
}

void WebRtcAudioSender::sendJson(mg_connection* connection, const juce::String& json)
{
    if (connection == nullptr || json.isEmpty())
        return;

    mg_websocket_write(connection,
                       MG_WEBSOCKET_OPCODE_TEXT,
                       json.toRawUTF8(),
                       json.getNumBytesAsUTF8());
}

juce::String WebRtcAudioSender::stateName(int state)
{
    switch (static_cast<rtc::PeerConnection::State> (state))
    {
        case rtc::PeerConnection::State::New: return "new";
        case rtc::PeerConnection::State::Connecting: return "connecting";
        case rtc::PeerConnection::State::Connected: return "connected";
        case rtc::PeerConnection::State::Disconnected: return "disconnected";
        case rtc::PeerConnection::State::Failed: return "failed";
        case rtc::PeerConnection::State::Closed: return "closed";
    }

    return "unknown";
}

juce::String WebRtcAudioSender::iceStateName(int state)
{
    switch (static_cast<rtc::PeerConnection::IceState> (state))
    {
        case rtc::PeerConnection::IceState::New: return "new";
        case rtc::PeerConnection::IceState::Checking: return "checking";
        case rtc::PeerConnection::IceState::Connected: return "connected";
        case rtc::PeerConnection::IceState::Completed: return "completed";
        case rtc::PeerConnection::IceState::Failed: return "failed";
        case rtc::PeerConnection::IceState::Disconnected: return "disconnected";
        case rtc::PeerConnection::IceState::Closed: return "closed";
    }

    return "unknown";
}
}
