// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace mconnect
{
MConnectAudioProcessor::MConnectAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "MConnectState", createParameterLayout()), networkServer(tapFifo)
{
    // Allocate once, before the host callback and worker start.
    tapFifo.prepare(65536);
    for (const auto* id : {ParamIDs::streamEnabled})
        parameters.addParameterListener(id, this);
    setLatencySamples(0);
}
MConnectAudioProcessor::~MConnectAudioProcessor()
{
    tapEnabled.store(false, std::memory_order_release);
    cancelPendingUpdate();
    for (const auto* id : {ParamIDs::streamEnabled})
        parameters.removeParameterListener(id, this);
}
void MConnectAudioProcessor::prepareToPlay(double rate, int blockFrames)
{
    if (!std::isfinite(rate) || rate < 8000.0 || rate > 384000.0) rate = 48000.0;
    currentSampleRate.store(rate, std::memory_order_release);
    networkServer.setSessionSampleRate(rate, blockFrames);
    prepared.store(true, std::memory_order_release);
    tapEnabled.store(parameters.getRawParameterValue(ParamIDs::streamEnabled)->load() >= 0.5f, std::memory_order_release);
    triggerAsyncUpdate();
}
bool MConnectAudioProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    const auto& input = layout.getMainInputChannelSet();
    return input == layout.getMainOutputChannelSet()
        && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}
template <typename Sample> void MConnectAudioProcessor::process(juce::AudioBuffer<Sample>& buffer)
{
    juce::ScopedNoDenormals noDenormals;
    const auto channels = juce::jmin(buffer.getNumChannels(), getTotalNumInputChannels());
    if (channels < 1) return;
    if (editorCount.load(std::memory_order_relaxed) > 0) for (int c = 0; c < 2; ++c)
    {
        const auto* samples = buffer.getReadPointer(juce::jmin(c, channels - 1));
        float peak = 0.0f;
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto value = static_cast<float>(std::abs(samples[i]));
            if (std::isfinite(value)) peak = juce::jmax(peak, value);
        }
        peaks[c].store(peak, std::memory_order_relaxed);
    }
    if (tapEnabled.load(std::memory_order_relaxed) && networkServer.isListening() && !isNonRealtime())
        tapFifo.pushFromAudioThread(buffer, channels, buffer.getNumSamples());
    // Host buffer is untouched even if transmission fails or clips.
}
void MConnectAudioProcessor::processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) { process(b); }
void MConnectAudioProcessor::processBlock(juce::AudioBuffer<double>& b, juce::MidiBuffer&) { process(b); }
juce::AudioProcessorEditor* MConnectAudioProcessor::createEditor() { return new MConnectAudioProcessorEditor(*this); }
void MConnectAudioProcessor::handleAsyncUpdate()
{
    auto config = configFromState(parameters, currentSampleRate.load(std::memory_order_acquire));
    config.streamEnabled = config.streamEnabled && prepared.load(std::memory_order_acquire);
    tapEnabled.store(config.streamEnabled, std::memory_order_release);
    networkServer.applyConfig(config);
}
void MConnectAudioProcessor::getStateInformation(juce::MemoryBlock& data)
{
    if (auto xml = parameters.copyState().createXml()) copyXmlToBinary(*xml, data);
}
void MConnectAudioProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size); xml && xml->hasTagName(parameters.state.getType()))
    {
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
        triggerAsyncUpdate();
    }
}
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new mconnect::MConnectAudioProcessor(); }
