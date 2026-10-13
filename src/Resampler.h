#pragma once
#include <samplerate.h>
#include <vector>
namespace mconnect
{
class Resampler
{
public:
    Resampler();
    ~Resampler();
    void reset();
    void process(const float*, size_t, int sourceRate, int targetRate, std::vector<float>&);
private:
    SRC_STATE* state = nullptr;
    int previousSourceRate = 0, previousTargetRate = 0;
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
};
}
