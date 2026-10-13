// Derived from PGStream by Andrulius (AGPL-3.0-only). See NOTICE.md.
#include "HtmlAssets.h"
#include <BinaryData.h>
#include <cstring>
namespace mconnect
{
bool getHtmlAsset(const char* uri, HtmlAsset& asset)
{
    if (!uri || std::strcmp(uri, "/") == 0 || std::strcmp(uri, "/index.html") == 0)
        asset = {MConnectBinaryData::index_html, static_cast<size_t>(MConnectBinaryData::index_htmlSize), "text/html; charset=utf-8"};
    else if (std::strcmp(uri, "/app.js") == 0)
        asset = {MConnectBinaryData::app_js, static_cast<size_t>(MConnectBinaryData::app_jsSize), "application/javascript; charset=utf-8"};
    else if (std::strcmp(uri, "/style.css") == 0)
        asset = {MConnectBinaryData::style_css, static_cast<size_t>(MConnectBinaryData::style_cssSize), "text/css; charset=utf-8"};
    else if (std::strcmp(uri, "/source.zip") == 0)
        asset = {MConnectBinaryData::source_zip, static_cast<size_t>(MConnectBinaryData::source_zipSize), "application/zip"};
    else if (std::strcmp(uri, "/license") == 0)
        asset = {MConnectBinaryData::LICENSE, static_cast<size_t>(MConnectBinaryData::LICENSESize), "text/plain; charset=utf-8"};
    else return false;
    return true;
}
}
