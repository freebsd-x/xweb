#pragma once

// What encoding a page is in, and turning it into the UTF-8 the parser expects.

#include <string>
#include <string_view>

namespace fbx::xweb {

// From a Content-Type header's charset, else a byte order mark, else a <meta> near the
// start, else UTF-8. Windows-1252 family, UTF-16 and EUC-KR are converted; encodings we have no
// table for (Shift_JIS, GBK...) are passed through and come out garbled.
[[nodiscard]] std::string decodeToUtf8(std::string_view bytes, std::string_view content_type = {});

// The charset decodeToUtf8 would go by (as labelled, lower case; empty if none is said).
[[nodiscard]] std::string detectCharset(std::string_view bytes, std::string_view content_type = {});

// EUC-KR (windows-949) as the Encoding Standard decodes it, and the labels that name it
// (ks_c_5601-1987 and the rest).
[[nodiscard]] bool isEucKrLabel(std::string_view label);
[[nodiscard]] std::string eucKrToUtf8(std::string_view bytes);
// UTF-8 text in EUC-KR, what it has no code for as &#n; (as forms submit it).
[[nodiscard]] std::string utf8ToEucKr(std::string_view text);

}  // namespace fbx::xweb
