// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "PluginState.h"
namespace mconnect
{
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{ParamIDs::streamEnabled, 1}, "Stream", true));
    return {params.begin(), params.end()};
}
StreamConfig configFromState(const juce::AudioProcessorValueTreeState& state, double sampleRate)
{
    StreamConfig result;
    result.streamEnabled = state.getRawParameterValue(ParamIDs::streamEnabled)->load() >= 0.5f;
    result.sessionSampleRate = sampleRate > 1.0 ? sampleRate : 48000.0;
    result.useSelfSignedCertificate = false;
    result.audioPassthrough = true;
    result.transportMode = result.selectedTransportMode = TransportMode::opusWebRtc;
    result.opusBitrateMode = OpusBitrateMode::highQuality320;
    result.latencyMode = LatencyMode::lowLatency;
    return result;
}
}
