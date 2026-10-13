// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include <juce_core/juce_core.h>

namespace mconnect
{
struct CertificateFiles
{
    bool ready = false;
    juce::File pemFile;
    juce::String description;
};

CertificateFiles ensureSelfSignedCertificate(const juce::String& lanIpAddress);
}
