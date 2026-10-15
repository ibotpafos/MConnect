#include "PluginUpdates.h"

#if !JUCE_MAC
namespace mconnect::updates
{
Reply fetch(const Request&, const Cancelled&, const Progress&)
{
    // The current release/install pipeline ships macOS AU/VST3 only.
    return {0, {}, "Updates are currently available on macOS"};
}
}
#endif
