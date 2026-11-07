#include <gtest/gtest.h>

#include "xweb/url.h"

using namespace fbx::xweb;

TEST(Url, Parses) {
    const Url u = parseUrl("HTTPS://Example.COM:8443/a/b?x=1#top");
    EXPECT_EQ(u.scheme, "https");
    EXPECT_EQ(u.authority, "example.com:8443");
    EXPECT_EQ(u.path, "/a/b");
    EXPECT_EQ(u.query, "x=1");
    EXPECT_EQ(u.fragment, "top");
    EXPECT_EQ(u.withoutFragment(), "https://example.com:8443/a/b?x=1");
    EXPECT_FALSE(parseUrl("a/b:c").isAbsolute());
    EXPECT_TRUE(parseUrl("mailto:a@b").isAbsolute());
}

// RFC 3986 5.4's own examples.
TEST(Url, ResolvesTheRfcExamples) {
    const char* base = "http://a/b/c/d;p?q";
    const std::pair<const char*, const char*> cases[] = {
        {"g:h", "g:h"}, {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"}, {"/g", "http://a/g"},
        {"//g", "http://g"}, {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"}, {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"}, {"g?y#s", "http://a/b/c/g?y#s"}, {";x", "http://a/b/c/;x"}, {"g;x", "http://a/b/c/g;x"},
        {"", "http://a/b/c/d;p?q"}, {".", "http://a/b/c/"}, {"./", "http://a/b/c/"}, {"..", "http://a/b/"}, {"../", "http://a/b/"},
        {"../g", "http://a/b/g"}, {"../..", "http://a/"}, {"../../", "http://a/"}, {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"}, {"/./g", "http://a/g"}, {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."}, {".g", "http://a/b/c/.g"}, {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"}, {"./g/.", "http://a/b/c/g/"}, {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
    };
    for (const auto& [ref, want] : cases) EXPECT_EQ(resolveUrl(base, ref), want) << ref;
}

TEST(Url, RelativeBases) {
    EXPECT_EQ(resolveUrl("css/main.css", "fonts.css"), "css/fonts.css");
    EXPECT_EQ(resolveUrl("../_static/a.css", "b.css"), "../_static/b.css");
    EXPECT_EQ(resolveUrl("", "a.css"), "a.css");
    EXPECT_EQ(resolveUrl("x/y.css", "../z.css"), "z.css");
    EXPECT_EQ(resolveUrl("file:///usr/doc/a.html", "b.html#c"), "file:///usr/doc/b.html#c");
    EXPECT_EQ(resolveUrl("http://a/b", "http://c"), "http://c/");
    EXPECT_EQ(resolveUrl("http://a/b", "  c\n"), "http://a/c");
}

TEST(DataUrl, Decodes) {
    std::string body, type;
    ASSERT_TRUE(decodeDataUrl("data:font/woff2;base64,aGVsbG8=", body, type));
    EXPECT_EQ(body, "hello");
    EXPECT_EQ(type, "font/woff2");
    ASSERT_TRUE(decodeDataUrl("DATA:text/plain,a%20b", body, type));
    EXPECT_EQ(body, "a b");
    ASSERT_TRUE(decodeDataUrl("data:,x", body, type));
    EXPECT_EQ(type, "text/plain");
    ASSERT_TRUE(decodeDataUrl("data:;base64,aGk%3D", body, type));
    EXPECT_EQ(body, "hi");
    EXPECT_FALSE(decodeDataUrl("data:nocomma", body, type));
    EXPECT_FALSE(decodeDataUrl("data:;base64,!!!", body, type));
    EXPECT_FALSE(decodeDataUrl("http://a/b", body, type));
    EXPECT_FALSE(decodeDataUrl("", body, type));
}

TEST(DataUrl, ComesThroughResolvingAsIs) {
    const std::string data = "data:font/woff;base64,AA+/AA==";
    EXPECT_EQ(resolveUrl("http://a/b.css", data), data);
}

TEST(Url, SameSiteForCookies) {
    EXPECT_TRUE(sameSite("https://www.example.com/a", "https://img.example.com/b.png"));
    EXPECT_TRUE(sameSite("http://example.com/", "https://cdn.example.com/x"));
    EXPECT_FALSE(sameSite("https://example.com/", "https://example.org/"));
    EXPECT_TRUE(sameSite("https://www.naver.co.kr/", "https://news.naver.co.kr/"));
    EXPECT_FALSE(sameSite("https://a.co.kr/", "https://b.co.kr/"));
    EXPECT_FALSE(sameSite("https://alice.github.io/", "https://bob.github.io/"));
    EXPECT_TRUE(sameSite("http://127.0.0.1:8000/", "http://127.0.0.1:9000/x"));
    EXPECT_FALSE(sameSite("http://127.0.0.1/", "http://localhost/"));
    EXPECT_FALSE(sameSite("file:///a.html", "file:///b.html"));
    EXPECT_FALSE(sameSite("https://example.com/", "data:text/plain,x"));
    EXPECT_EQ(registrableDomain("WWW.Example.COM."), "example.com");
    EXPECT_EQ(registrableDomain("bbc.co.uk"), "bbc.co.uk");
    EXPECT_EQ(registrableDomain("localhost"), "localhost");
}
