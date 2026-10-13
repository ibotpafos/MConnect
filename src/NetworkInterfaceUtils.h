// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include <juce_core/juce_core.h>

namespace mconnect
{
struct NetworkInterfaceSelection
{
    juce::String primaryAddress;
    juce::String primaryUrl;
    juce::StringArray candidateUrls;
    juce::StringArray candidateDescriptions;
};

NetworkInterfaceSelection selectBestLanInterface(int port, bool useHttps);
}
