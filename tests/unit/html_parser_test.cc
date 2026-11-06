// What the html5lib suite doesn't check: our limits and the serializer.

#include <gtest/gtest.h>

#include <string>

#include "xweb/html_parser.h"

using namespace fbx::xweb;

TEST(HtmlParser, ParsesADocument) {
    const auto doc = parseHtml("<!doctype html><title>Hi</title><p class=a>one<p>two");
    ASSERT_NE(doc->body(), nullptr);
    EXPECT_FALSE(doc->quirks);
    EXPECT_EQ(doc->head()->children[0]->textContent(), "Hi");
    ASSERT_EQ(doc->body()->children.size(), 2u);
    EXPECT_EQ(*doc->body()->children[0]->attr("class"), "a");
    EXPECT_EQ(doc->body()->children[1]->textContent(), "two");
}

TEST(HtmlParser, NoDoctypeMeansQuirks) { EXPECT_TRUE(parseHtml("<p>x")->quirks); }

TEST(HtmlParser, SerializesAsABrowserWould) {
    EXPECT_EQ(serializeDocument(*parseHtml("<!doctype html><a href='x&amp;y\"z'>a &lt; b\xC2\xA0</a><br><style>a>b{}</style>")),
              "<!DOCTYPE html><html><head></head><body><a href=\"x&amp;y&quot;z\">a &lt; b&nbsp;</a><br><style>a>b{}</style></body></html>");
}

TEST(HtmlParser, NestingStopsAtTheLimit) {
    std::string html;
    for (int i = 0; i < 200000; ++i) html += "<div>";
    const auto doc = parseHtml(html);
    size_t depth = 0;
    for (const Node* n = doc->body(); n && !n->children.empty(); n = n->children.back()) ++depth;
    EXPECT_LE(depth, kMaxOpenElements + 8);
}

TEST(HtmlParser, InvalidUtf8BecomesReplacementCharacters) {
    const auto doc = parseHtml("<p>a\xFF\xC0\x80z");
    EXPECT_EQ(doc->body()->textContent(), "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDz");
}

TEST(HtmlParser, EndOfInputInsideATagDropsIt) {
    const auto doc = parseHtml("<p>x<div class=");
    EXPECT_EQ(serializeChildren(*doc->body()), "<p>x</p>");
}
