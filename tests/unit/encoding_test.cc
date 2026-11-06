#include <gtest/gtest.h>

#include "xweb/encoding.h"

using fbx::xweb::decodeToUtf8;

TEST(Encoding, Utf8IsTheDefault) {
    EXPECT_EQ(decodeToUtf8("caf\xC3\xA9"), "caf\xC3\xA9");
    EXPECT_EQ(decodeToUtf8("\xEF\xBB\xBFx"), "x");
}

TEST(Encoding, Latin1MeansWindows1252) {
    EXPECT_EQ(decodeToUtf8("caf\xE9", "text/html; charset=ISO-8859-1"), "caf\xC3\xA9");
    EXPECT_EQ(decodeToUtf8("\x93q\x94", "text/html;charset=windows-1252"), "\xE2\x80\x9Cq\xE2\x80\x9D");
    EXPECT_EQ(decodeToUtf8("<meta charset=latin1>\xE9"), "<meta charset=latin1>\xC3\xA9");
    EXPECT_EQ(decodeToUtf8("<meta http-equiv=content-type content='text/html; charset=iso-8859-1'>\xE9"),
              "<meta http-equiv=content-type content='text/html; charset=iso-8859-1'>\xC3\xA9");
}

TEST(Encoding, TheHeaderBeatsTheMeta) {
    EXPECT_EQ(decodeToUtf8("<meta charset=latin1>\xC3\xA9", "text/html; charset=utf-8"), "<meta charset=latin1>\xC3\xA9");
}

TEST(Encoding, Utf16) {
    EXPECT_EQ(decodeToUtf8(std::string_view("\xFF\xFEh\0i\0", 6)), "hi");
    EXPECT_EQ(decodeToUtf8(std::string_view("\xFE\xFF\0h\0i", 6)), "hi");
    EXPECT_EQ(decodeToUtf8(std::string_view("\xFF\xFE\x3D\xD8\x00\xDE", 6)), "\xF0\x9F\x98\x80");
    EXPECT_EQ(decodeToUtf8(std::string_view("\xFF\xFE\x3D\xD8", 4)), "\xEF\xBF\xBD");
}
