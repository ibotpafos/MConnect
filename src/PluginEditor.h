#pragma once
#include "PluginProcessor.h"
#include "PluginUpdates.h"
#include <juce_gui_basics/juce_gui_basics.h>
namespace mconnect
{
class MConnectAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MConnectAudioProcessorEditor(MConnectAudioProcessor&);
    ~MConnectAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;
    MConnectAudioProcessor& owner;
    juce::LookAndFeel_V4 look;
    juce::TextButton stream {"Stop"}, copy {"Copy link"}, update;
    juce::SharedResourcePointer<updates::Service> updater;
    juce::ScopedMessageBox installPrompt;
    juce::String updateText;
    int updateTicks = 0;
    bool installWhenReady = false;
    juce::Image qr;
    juce::String pairingUrl, currentStatus;
    bool running = false;
    float displayedPeaks[2] {};
    int copyTicks = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MConnectAudioProcessorEditor)
};
}
