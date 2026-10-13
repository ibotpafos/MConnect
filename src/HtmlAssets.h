// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#pragma once

#include <cstddef>

namespace mconnect
{
struct HtmlAsset
{
    const char* data = nullptr;
    size_t size = 0;
    const char* mimeType = "application/octet-stream";
};

bool getHtmlAsset(const char* uri, HtmlAsset& asset);
}
