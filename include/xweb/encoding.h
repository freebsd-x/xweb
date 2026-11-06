#pragma once

// What encoding a page is in, and turning it into the UTF-8 the parser expects.

#include <string>
#include <string_view>

namespace fbx::xweb {

// From a Content-Type header's charset, else a byte order mark, else a <meta> near the
// start, else UTF-8. Windows-1252 family and UTF-16 are converted; encodings we have no
// table for (Shift_JIS, EUC-KR, GBK...) are passed through and come out garbled.
[[nodiscard]] std::string decodeToUtf8(std::string_view bytes, std::string_view content_type = {});

}  // namespace fbx::xweb
