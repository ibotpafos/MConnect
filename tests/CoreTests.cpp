#include "PluginProcessor.h"
#include "Resampler.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <utility>
#include <rtc/utils.hpp>

using namespace mconnect;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <typename Sample> void passthrough()
{
    MConnectAudioProcessor processor;
    processor.prepareToPlay(44100, 513);
    juce::AudioBuffer<Sample> buffer(2, 513);
    juce::Random random(903);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 513; ++i)
        buffer.setSample(c, i, static_cast<Sample>(random.nextDouble() * 3.0 - 1.5));
    buffer.setSample(0, 0, std::numeric_limits<Sample>::quiet_NaN());
    buffer.setSample(1, 0, std::numeric_limits<Sample>::infinity());
    juce::AudioBuffer<Sample> original; original.makeCopyOf(buffer);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    for (int c = 0; c < 2; ++c)
        require(std::memcmp(original.getReadPointer(c), buffer.getReadPointer(c), 513 * sizeof(Sample)) == 0, "host audio changed");
    require(processor.getLatencySamples() == 0, "nonzero DAW latency");
    const auto queued = processor.getStreamStats().senderQueueFillFrames;
    processor.setNonRealtime(true); processor.processBlock(buffer, midi);
    require(processor.getStreamStats().senderQueueFillFrames == queued, "offline render streamed");
    processor.processBlockBypassed(buffer, midi);
    for (int c = 0; c < 2; ++c)
        require(std::memcmp(original.getReadPointer(c), buffer.getReadPointer(c), 513 * sizeof(Sample)) == 0, "bypass changed audio");
    auto layout = processor.getBusesLayout();
    layout.inputBuses.set(0, juce::AudioChannelSet::mono()); layout.outputBuses.set(0, juce::AudioChannelSet::mono());
    require(processor.setBusesLayout(layout), "mono unsupported");
    layout.inputBuses.set(0, juce::AudioChannelSet::create5point1()); layout.outputBuses.set(0, juce::AudioChannelSet::create5point1());
    require(!processor.isBusesLayoutSupported(layout), "unadvertised surround accepted");
    processor.parameters.getParameter(ParamIDs::streamEnabled)->setValueNotifyingHost(0.0f);
    juce::MemoryBlock saved; processor.getStateInformation(saved);
    MConnectAudioProcessor restored; restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    require(restored.parameters.getRawParameterValue(ParamIDs::streamEnabled)->load() < 0.5f, "stream state not restored");
    processor.prepareToPlay(std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<int>::max());
    require(processor.getStreamStats().inputSampleRate == 48000, "invalid host configuration not sanitized");
}
static void fifoVerification()
{
    AudioTapFifo fifo; fifo.prepare(4096);
    juce::AudioBuffer<float> mono(1, 8);
    for (int i = 0; i < 8; ++i) mono.setSample(0, i, static_cast<float>(i));
    mono.setSample(0, 3, std::numeric_limits<float>::quiet_NaN());
    fifo.pushFromAudioThread(mono, 1, 8);
    float received[16] {};
    require(fifo.pop(received, 8) == 8, "FIFO count");
    for (int i = 0; i < 8; ++i) require(received[2*i] == received[2*i+1] && std::isfinite(received[2*i]), "mono or finite conversion");
    std::atomic<bool> done {false}, corrupted {false};
    std::thread producer([&]
    {
        juce::AudioBuffer<float> block(2, 32);
        for (int start = 1; start < 2000000; start += 32)
        {
            for (int i = 0; i < 32; ++i) { block.setSample(0, i, static_cast<float>(start+i)); block.setSample(1, i, -static_cast<float>(start+i)); }
            fifo.pushFromAudioThread(block, 2, 32);
        }
        done.store(true, std::memory_order_release);
    });
    std::thread consumer([&]
    {
        float samples[74]; float previous = 0; int rounds = 0;
        while (!done.load(std::memory_order_acquire) || fifo.getAvailableFrames() > 0)
        {
            const auto frames = fifo.pop(samples, 37);
            for (size_t i = 0; i < frames; ++i)
            {
                if (samples[i*2] != -samples[i*2+1] || samples[i*2] <= previous) corrupted.store(true);
                previous = samples[i*2];
            }
            if (++rounds % 7 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    producer.join(); consumer.join();
    require(!corrupted.load(), "concurrent FIFO samples corrupted");
    require(fifo.getDroppedFrames() > 0, "overflow path untested");
    fifo.pushFromAudioThread(mono, 1, 8); fifo.discardFromConsumer(100);
    require(fifo.getAvailableFrames() == 0, "consumer discard");
}
static void callbackResetSafety()
{
    rtc::synchronized_stored_callback<> callback;
    auto reset = [&] { for (int i = 0; i < 4000; ++i) callback = nullptr; };
    std::thread first(reset), second(reset);
    for (int i = 0; i < 4000; ++i) callback();
    first.join(); second.join();
    callback = nullptr;
    callback();
    int replayed = 0;
    callback = std::function<void()>([&] { ++replayed; });
    require(replayed == 1, "stored callback did not replay its missed event");
    callback = nullptr;
    callback = std::function<void()>([&] { ++replayed; });
    require(replayed == 1, "callback reset retained an obsolete event");
}
static void stateRestoration()
{
    MConnectAudioProcessor processor;
    auto* parameter = processor.parameters.getParameter(ParamIDs::streamEnabled);
    require(parameter->isBoolean() && parameter->isDiscrete() && parameter->getNumSteps() == 2,
            "stream switch host metadata changed");
    for (const auto values : {std::pair{0.340869f, 0.0489689f}, std::pair{0.8f, 0.52f},
                             std::pair{0.499f, 0.501f}, std::pair{0.5f, 0.0f}, std::pair{1.0f, 0.0f}})
    {
        parameter->setValueNotifyingHost(values.first);
        const auto expected = parameter->getValue();
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        parameter->setValueNotifyingHost(values.second);
        processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        require(parameter->getValue() == expected, "fractional automation state was not restored");
        require(configFromState(processor.parameters, 48000).streamEnabled == (values.first >= 0.5f),
                "restored Stream value changed its on/off behavior");
    }
    // Presets saved before normalized-value preservation contain only APVTS.
    parameter->setValueNotifyingHost(0.0f);
    auto legacyXml = processor.parameters.copyState().createXml();
    juce::MemoryBlock legacy;
    juce::AudioProcessor::copyXmlToBinary(*legacyXml, legacy);
    parameter->setValueNotifyingHost(1.0f);
    processor.setStateInformation(legacy.getData(), static_cast<int>(legacy.getSize()));
    require(parameter->getValue() == 0.0f, "legacy Stream preset was not restored");
}
static double amplitude(const std::vector<float>& samples, double frequency, int channel)
{
    const size_t begin = 3000, frames = samples.size()/2;
    double real = 0, imaginary = 0;
    for (size_t i = begin; i < frames; ++i)
    {
        const double angle = 2.0 * juce::MathConstants<double>::pi * frequency * static_cast<double>(i) / 48000;
        real += samples[i*2+static_cast<size_t>(channel)] * std::cos(angle);
        imaginary += samples[i*2+static_cast<size_t>(channel)] * std::sin(angle);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(frames-begin);
}
static void resampling()
{
    for (int rate : {32000, 44100, 48000, 88200, 96000, 192000})
    {
        Resampler resampler;
        std::vector<float> input(317*2), output, collected;
        for (int start = 0; start < rate; start += 317)
        {
            const int count = juce::jmin(317, rate-start);
            for (int i = 0; i < count; ++i)
            {
                input[static_cast<size_t>(i)*2] = static_cast<float>(0.25 * std::sin(2 * juce::MathConstants<double>::pi * 997 * (start+i) / rate));
                input[static_cast<size_t>(i)*2+1] = static_cast<float>(0.25 * std::sin(2 * juce::MathConstants<double>::pi * 1499 * (start+i) / rate));
            }
            resampler.process(input.data(), static_cast<size_t>(count), rate, 48000, output);
            collected.insert(collected.end(), output.begin(), output.end());
        }
        require(std::abs(static_cast<int>(collected.size()/2)-48000) < 300, "SRC frame drift");
        require(std::abs(amplitude(collected,997,0)-0.25) < 0.004, "left SRC tone damaged");
        require(std::abs(amplitude(collected,1499,1)-0.25) < 0.004, "right SRC tone damaged");
        require(amplitude(collected,1499,0) < 0.001 && amplitude(collected,997,1) < 0.001, "SRC stereo crosstalk");
    }
    Resampler downsample;
    std::vector<float> input(96000*2), output;
    for (int i = 0; i < 96000; ++i) input[static_cast<size_t>(i)*2] = input[static_cast<size_t>(i)*2+1] = static_cast<float>(std::sin(2 * juce::MathConstants<double>::pi * 31000 * i / 96000));
    downsample.process(input.data(), 96000, 96000, 48000, output);
    require(amplitude(output,17000,0) < 0.0003, "alias suppression failed");

    // Returning through a 48 kHz passthrough must not reuse old SRC history.
    Resampler switched, fresh;
    std::vector<float> silence(4096 * 2), reference;
    switched.process(silence.data(),4096,44100,48000,output);
    switched.process(silence.data(),4096,48000,48000,output);
    switched.process(input.data(),4096,44100,48000,output);
    fresh.process(input.data(),4096,44100,48000,reference);
    require(output == reference, "SRC reused stale history after passthrough");
}
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    try { passthrough<float>(); passthrough<double>(); stateRestoration(); callbackResetSafety(); fifoVerification(); resampling(); }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    std::cout << "PASS: passthrough float/double, bypass/offline, mono/state, concurrent overflow, stereo SRC and alias rejection\n";
    return 0;
}
