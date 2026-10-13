// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once
#include "AudioTapFifo.h"
#include "NetworkServer.h"
#include "PluginState.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

namespace mconnect
{
class MConnectAudioProcessor final : public juce::AudioProcessor,
                                    private juce::AudioProcessorValueTreeState::Listener,
                                    private juce::AsyncUpdater
{
public:
    MConnectAudioProcessor();
    ~MConnectAudioProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override { tapEnabled.store(false, std::memory_order_release); }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    void processBlockBypassed(juce::AudioBuffer<double>&, juce::MidiBuffer&) override {}
    bool supportsDoublePrecisionProcessing() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    juce::AudioProcessorValueTreeState parameters;
    StreamStats getStreamStats() const { return networkServer.getStats(); }
    float getPeak(int channel) const { return peaks[channel == 0 ? 0 : 1].load(std::memory_order_relaxed); }
    void editorOpened() noexcept { editorCount.fetch_add(1, std::memory_order_relaxed); }
    void editorClosed() noexcept { editorCount.fetch_sub(1, std::memory_order_relaxed); }
    juce::String getPairingUrl(const juce::String& address = {}) const { return networkServer.getPairingUrl(address); }
private:
    template <typename Sample> void process(juce::AudioBuffer<Sample>&);
    void parameterChanged(const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    AudioTapFifo tapFifo;
    NetworkServer networkServer;
    std::atomic<double> currentSampleRate {48000.0};
    std::atomic<bool> tapEnabled {false};
    std::atomic<bool> prepared {false};
    std::atomic<float> peaks[2] {};
    std::atomic<int> editorCount {0};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MConnectAudioProcessor)
};
}
