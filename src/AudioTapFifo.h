// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace mconnect
{
class AudioTapFifo
{
public:
    void prepare(size_t requestedFrames);
    void reset();

    void pushFromAudioThread(const juce::AudioBuffer<float>& buffer, int inputChannels, int frameCount) noexcept;
    void pushFromAudioThread(const juce::AudioBuffer<double>& buffer, int inputChannels, int frameCount) noexcept;
    size_t pop(float* destinationInterleavedStereo, size_t maxFrames) noexcept;
    // Only the consumer may move readIndex. Do not reset/prepare while in use.
    void discardFromConsumer(size_t frames) noexcept;

    uint64_t getDroppedFrames() const noexcept { return droppedFrames.load(std::memory_order_relaxed); }
    size_t getCapacityFrames() const noexcept { return capacityFrames; }
    size_t getAvailableFrames() const noexcept;

private:
    template <typename Sample> void push(const juce::AudioBuffer<Sample>&, int, int) noexcept;
    size_t capacityFrames = 0;
    size_t capacityMask = 0;
    std::vector<float> samples;
    std::atomic<uint64_t> readIndex { 0 };
    std::atomic<uint64_t> writeIndex { 0 };
    std::atomic<uint64_t> droppedFrames { 0 };
};
}
