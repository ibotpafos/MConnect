// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "AudioTapFifo.h"
#include <cmath>

namespace mconnect
{
static size_t nextPowerOfTwo(size_t value)
{
    size_t result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

void AudioTapFifo::prepare(size_t requestedFrames)
{
    capacityFrames = nextPowerOfTwo(std::max<size_t> (requestedFrames, 4096));
    capacityMask = capacityFrames - 1;
    samples.assign(capacityFrames * 2, 0.0f);
    reset();
}

void AudioTapFifo::reset()
{
    readIndex.store(0, std::memory_order_release);
    writeIndex.store(0, std::memory_order_release);
    droppedFrames.store(0, std::memory_order_release);
}

size_t AudioTapFifo::getAvailableFrames() const noexcept
{
    const auto read = readIndex.load(std::memory_order_acquire);
    const auto write = writeIndex.load(std::memory_order_acquire);
    return write >= read ? std::min<size_t> (capacityFrames, static_cast<size_t> (write - read)) : 0;
}

void AudioTapFifo::pushFromAudioThread(const juce::AudioBuffer<float>& buffer, int inputChannels, int frameCount) noexcept
{
    push(buffer, inputChannels, frameCount);
}

void AudioTapFifo::pushFromAudioThread(const juce::AudioBuffer<double>& buffer, int inputChannels, int frameCount) noexcept
{
    push(buffer, inputChannels, frameCount);
}

template <typename Sample>
void AudioTapFifo::push(const juce::AudioBuffer<Sample>& buffer, int inputChannels, int frameCount) noexcept
{
    if (capacityFrames == 0 || frameCount <= 0 || inputChannels <= 0)
        return;

    const auto framesToWrite = static_cast<size_t> (juce::jmin(frameCount, buffer.getNumSamples()));
    const auto write = writeIndex.load(std::memory_order_relaxed);
    const auto read = readIndex.load(std::memory_order_acquire);
    const auto used = static_cast<size_t> (write - read);
    if (framesToWrite > capacityFrames - used)
    {
        // Never overwrite samples the consumer is reading.
        droppedFrames.fetch_add(framesToWrite, std::memory_order_relaxed);
        return;
    }

    const auto* left = buffer.getReadPointer(0);
    const auto* right = inputChannels > 1 ? buffer.getReadPointer(1) : left;

    for (size_t i = 0; i < framesToWrite; ++i)
    {
        const auto destination = static_cast<size_t> ((write + i) & capacityMask) * 2;
        const auto l = static_cast<float> (left[i]);
        const auto r = static_cast<float> (right[i]);
        samples[destination] = std::isfinite(l) ? l : 0.0f;
        samples[destination + 1] = std::isfinite(r) ? r : 0.0f;
    }

    writeIndex.store(write + framesToWrite, std::memory_order_release);
}

size_t AudioTapFifo::pop(float* destinationInterleavedStereo, size_t maxFrames) noexcept
{
    if (capacityFrames == 0 || maxFrames == 0 || destinationInterleavedStereo == nullptr)
        return 0;

    auto read = readIndex.load(std::memory_order_relaxed);
    const auto write = writeIndex.load(std::memory_order_acquire);
    const auto available = std::min<size_t> (maxFrames, static_cast<size_t> (write - read));

    for (size_t i = 0; i < available; ++i)
    {
        const auto source = static_cast<size_t> ((read + i) & capacityMask) * 2;
        destinationInterleavedStereo[i * 2] = samples[source];
        destinationInterleavedStereo[i * 2 + 1] = samples[source + 1];
    }

    readIndex.store(read + available, std::memory_order_release);
    return available;
}

void AudioTapFifo::discardFromConsumer(size_t frames) noexcept
{
    const auto read = readIndex.load(std::memory_order_relaxed);
    const auto write = writeIndex.load(std::memory_order_acquire);
    readIndex.store(read + std::min<size_t> (frames, static_cast<size_t> (write - read)), std::memory_order_release);
}
}
