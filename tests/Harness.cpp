#include "PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <chrono>
#include <thread>
#if JUCE_MAC
#import <Foundation/Foundation.h>
#include <mach/task_policy.h>
#endif

class ToneSource : public juce::Thread
{
public:
    ToneSource(mconnect::MConnectAudioProcessor& p, double sr, int frames) : Thread("Simulated DAW"), processor(p), rate(sr), block(frames) {}
    void setPaused(bool value) { paused.store(value); }
    void run() override
    {
        juce::AudioBuffer<float> audio(2, block); juce::MidiBuffer midi;
        uint64_t frame = 0; auto due = std::chrono::steady_clock::now();
        while (!threadShouldExit())
        {
            if (!paused.load()) for (int i = 0; i < block; ++i)
            {
                const auto t = static_cast<double>(frame + static_cast<uint64_t>(i)) / rate;
                audio.setSample(0, i, static_cast<float>(0.25 * std::sin(2 * juce::MathConstants<double>::pi * 997 * t)));
                audio.setSample(1, i, static_cast<float>(0.25 * std::sin(2 * juce::MathConstants<double>::pi * 1499 * t)));
            }
            if (!paused.load()) processor.processBlock(audio, midi);
            frame += static_cast<uint64_t>(block);
            due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(block / rate));
            std::this_thread::sleep_until(due);
        }
    }
private:
    mconnect::MConnectAudioProcessor& processor; double rate; int block;
    std::atomic<bool> paused {false};
};
class Harness final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "MConnect test host"; }
    const juce::String getApplicationVersion() override { return MCONNECT_VERSION; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String& commandLine) override
    {
#if JUCE_MAC
        task_category_policy_data_t category {TASK_FOREGROUND_APPLICATION};
        task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
                        reinterpret_cast<task_policy_t>(&category), TASK_CATEGORY_POLICY_COUNT);
        // A real DAW holds an audio I/O activity. This software-only test source
        // must also keep its timers out of App Nap when its editor is hidden.
        testActivity = [[[NSProcessInfo processInfo] beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
            reason:@"MConnect simulated DAW audio validation"] retain];
#endif
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        int duration = 600;
        const bool showEditor = !args.contains("--no-editor");
        for (int i = 0; i+1 < args.size(); ++i)
        {
            if (args[i] == "--rate") rate = args[i+1].getDoubleValue();
            if (args[i] == "--block") block = args[i+1].getIntValue();
            if (args[i] == "--seconds") duration = args[i+1].getIntValue();
            if (args[i] == "--info") info = juce::File(args[i+1].unquoted());
            if (args[i] == "--control") control = juce::File(args[i+1].unquoted());
        }
        if (rate < 32000 || rate > 192000 || block < 1 || block > 8192) { quit(); return; }
        processor = std::make_unique<mconnect::MConnectAudioProcessor>(); startSource();
        if (showEditor) window = std::make_unique<Window>(processor->createEditor());
        deadline = juce::Time::getMillisecondCounterHiRes() + duration*1000.0;
        startTimer(250);
    }
    void shutdown() override
    {
        stopTimer(); if(tone) tone->stopThread(2000);
        window.reset(); tone.reset(); processor.reset();
        if(info != juce::File()) info.deleteFile();
#if JUCE_MAC
        if(testActivity) { [[NSProcessInfo processInfo] endActivity:testActivity]; [testActivity release]; testActivity = nil; }
#endif
    }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}
private:
    class Window : public juce::DocumentWindow
    {
    public:
        explicit Window(juce::AudioProcessorEditor* editor) : DocumentWindow("MConnect | test tones 997 Hz L / 1499 Hz R",juce::Colour(0xff101016),allButtons)
        { setUsingNativeTitleBar(true); setContentOwned(editor,true); centreWithSize(getWidth(),getHeight()); setVisible(true); }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    void timerCallback() override
    {
        if (control.existsAsFile())
        {
            const auto command = juce::JSON::parse(control.loadFileAsString());
            control.deleteFile();
            if (auto* object = command.getDynamicObject())
            {
                if (static_cast<bool>(object->getProperty("quit"))) { quit(); return; }
                if (object->hasProperty("stream")) processor->parameters.getParameter(mconnect::ParamIDs::streamEnabled)->setValueNotifyingHost(static_cast<bool>(object->getProperty("stream")) ? 1.0f : 0.0f);
                if (object->hasProperty("rate") || object->hasProperty("block"))
                {
                    tone->stopThread(2000);
                    if (object->hasProperty("rate")) rate = juce::jlimit(32000.0,192000.0,static_cast<double>(object->getProperty("rate")));
                    if (object->hasProperty("block")) block = juce::jlimit(1,8192,static_cast<int>(object->getProperty("block")));
                    startSource();
                }
                if (object->hasProperty("pause")) tone->setPaused(static_cast<bool>(object->getProperty("pause")));
            }
        }
        if(juce::Time::getMillisecondCounterHiRes() > deadline) { quit(); return; }
        if(info == juce::File() || !processor) return;
        auto object = std::make_unique<juce::DynamicObject>();
        auto stats = processor->getStreamStats();
        if(!stats.serverRunning) return;
        object->setProperty("url",processor->getPairingUrl()); object->setProperty("port",stats.port);
        object->setProperty("sampleRate",stats.inputSampleRate);
        object->setProperty("blockFrames",block);
        info.replaceWithText(juce::JSON::toString(juce::var(object.release())));
        info.setReadOnly(false);
    }
    void startSource()
    {
        processor->prepareToPlay(rate,block);
        tone = std::make_unique<ToneSource>(*processor,rate,block);
        const auto options = juce::Thread::RealtimeOptions{}.withPeriodMs(1000.0*block/rate).withProcessingTimeMs(0.05).withMaximumProcessingTimeMs(0.5);
        if (!tone->startRealtimeThread(options)) tone->startThread(juce::Thread::Priority::high);
    }
    juce::File info, control;
    double rate = 48000;
    int block = 256;
    double deadline = 0;
    std::unique_ptr<mconnect::MConnectAudioProcessor> processor;
    std::unique_ptr<ToneSource> tone;
    std::unique_ptr<Window> window;
#if JUCE_MAC
    id testActivity = nil;
#endif
};
START_JUCE_APPLICATION(Harness)
int main(int argc, char* argv[])
{
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main(argc, const_cast<const char**>(argv));
}
