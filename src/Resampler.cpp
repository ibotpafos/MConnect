#include "Resampler.h"
#include <cmath>
#include <stdexcept>
namespace mconnect
{
Resampler::Resampler() = default;
Resampler::~Resampler() { src_delete(state); }
void Resampler::reset() { if (state) src_reset(state); previousSourceRate = previousTargetRate = 0; }
void Resampler::process(const float* input, size_t frames, int sourceRate, int targetRate, std::vector<float>& output)
{
    output.clear();
    if (!input || frames == 0 || sourceRate <= 0 || targetRate <= 0) return;
    if (sourceRate != previousSourceRate || targetRate != previousTargetRate) reset();
    previousSourceRate = sourceRate; previousTargetRate = targetRate;
    if (sourceRate == targetRate) { output.assign(input, input + frames * 2); return; }
    if (!state)
    {
        int error = 0;
        state = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &error);
        if (!state) throw std::runtime_error(src_strerror(error));
    }
    const double ratio = static_cast<double>(targetRate) / sourceRate;
    size_t consumed = 0;
    do
    {
        const auto offset = output.size();
        const auto capacity = static_cast<size_t>(std::ceil((frames - consumed) * ratio)) + 512;
        output.resize(offset + capacity * 2);
        SRC_DATA data {};
        data.data_in = input + consumed * 2;
        data.data_out = output.data() + offset;
        data.input_frames = static_cast<long>(frames - consumed);
        data.output_frames = static_cast<long>(capacity);
        data.src_ratio = ratio;
        if (const auto error = src_process(state, &data); error != 0) throw std::runtime_error(src_strerror(error));
        consumed += static_cast<size_t>(data.input_frames_used);
        output.resize(offset + static_cast<size_t>(data.output_frames_gen) * 2);
        if (data.input_frames_used == 0 && data.output_frames_gen == 0) break;
    } while (consumed < frames);
}
}
