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

TEST(HtmlParser, SummaryClicksToggleTheirDetails) {
    auto doc = parseHtml("<details id=d><summary id=s><b id=b>x</b><a id=a href=#>l</a></summary><summary id=t>2</summary><p id=p>y</p></details>");
    const auto find = [&](const char* id) {
        std::vector<Node*> stack{doc->root()};
        while (!stack.empty()) {
            Node* n = stack.back();
            stack.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) return n;
            for (Node* c : n->children) stack.push_back(c);
        }
        return static_cast<Node*>(nullptr);
    };
    EXPECT_EQ(detailsToggledBy(find("b")), find("d"));
    EXPECT_EQ(detailsToggledBy(find("s")), find("d"));
    EXPECT_EQ(detailsToggledBy(find("a")), nullptr);   // the link takes it
    EXPECT_EQ(detailsToggledBy(find("t")), nullptr);   // not the first summary
    EXPECT_EQ(detailsToggledBy(find("p")), nullptr);
}
