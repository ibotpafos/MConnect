// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include "StreamTypes.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace mconnect
{
namespace ParamIDs
{
static constexpr const char* streamEnabled = "streamEnabled";
static constexpr const char* httpsPort = "httpsPort";
static constexpr const char* transportMode = "transportMode";
static constexpr const char* opusBitrate = "opusBitrate";
static constexpr const char* latencyMode = "latencyMode";
static constexpr const char* keepAlive = "keepAliveWhenIdle";
static constexpr const char* useSelfSignedCertificate = "useSelfSignedCertificate";
static constexpr const char* audioPassthrough = "audioPassthrough";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
StreamConfig configFromState(const juce::AudioProcessorValueTreeState& state, double sessionSampleRate);
}
