#pragma once
#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <gdkit/LookAndFeel.h>
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
    gdkit::LookAndFeel look {gdkit::Product::mconnect};
    juce::TextButton stream {"Stop"}, copy {"Copy link"};
    juce::Label version;
    juce::TooltipWindow tooltips {this, 600};
    juce::Image qr;
    juce::String pairingUrl, currentStatus;
    bool running = false;
    float displayedPeaks[2] {};
    int copyTicks = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MConnectAudioProcessorEditor)
};
}
