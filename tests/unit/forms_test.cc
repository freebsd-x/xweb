// Form state and submission: what a form sends, against the HTML standard.

#include <gtest/gtest.h>

#include <string>

#include "xweb/forms.h"
#include "xweb/html_parser.h"

using namespace fbx::xweb;

namespace {

const Node* byId(const Document& doc, const std::string& id) {
    std::vector<const Node*> todo{doc.root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (n->isElement())
            if (const std::string* v = n->attr("id"); v && *v == id) return n;
        todo.insert(todo.end(), n->children.begin(), n->children.end());
    }
    return nullptr;
}

}  // namespace

TEST(Forms, UrlEncoding) {
    EXPECT_EQ(urlEncodeForm({{"q", "a b&c=d"}, {"x", "*-._~"}}), "q=a+b%26c%3Dd&x=*-._%7E");
    EXPECT_EQ(urlEncodeForm({{"k", "한"}}), "k=%ED%95%9C");
}

TEST(Forms, GetSubmissionCollectsWhatIsOn) {
    const auto doc = parseHtml(R"H(<form id=f action="/search?old=1#top">
        <input name=q value="hello world">
        <input name=off disabled value=x>
        <input type=checkbox name=a checked><input type=checkbox name=b value=yes>
        <input type=radio name=r value=1><input type=radio name=r value=2 checked>
        <select name=s><option>One<option value=2 selected>Two</select>
        <textarea name=t>line1
line2</textarea>
        <input type=submit name=go value=Go id=go><input type=submit name=other value=No>
        <input type=hidden name=_charset_>
        <input name="">
        <fieldset disabled><input name=fs value=1></fieldset>
        </form>)H");
    FormState st;
    const auto sub = submitForm(byId(*doc, "f"), byId(*doc, "go"), st, "https://example.org/page");
    ASSERT_TRUE(sub);
    EXPECT_FALSE(sub->post);
    EXPECT_EQ(sub->url, "https://example.org/search?q=hello+world&a=on&r=2&s=2&t=line1%0D%0Aline2&go=Go&_charset_=UTF-8#top");
}

TEST(Forms, StateChangesWhatIsSent) {
    const auto doc = parseHtml(R"H(<form id=f action=/x><input id=q name=q value=a><input type=checkbox id=c name=c>
        <input type=radio id=r1 name=r value=1 checked><input type=radio id=r2 name=r value=2>
        <select id=s name=s><option>a<option>b<option>c</select></form>
        <form id=g><input type=radio id=r3 name=r value=3 checked></form>)H");
    FormState st;
    st.setValue(byId(*doc, "q"), "typed\nno newline");
    st.activate(byId(*doc, "c"));
    st.activate(byId(*doc, "r2"));
    st.select(byId(*doc, "s"), 2);
    EXPECT_FALSE(st.checked(byId(*doc, "r1")));
    EXPECT_TRUE(st.checked(byId(*doc, "r3")));   // another form's group
    auto sub = submitForm(byId(*doc, "f"), nullptr, st, "http://h/");
    ASSERT_TRUE(sub);
    EXPECT_EQ(sub->url, "http://h/x?q=typedno+newline&c=on&r=2&s=c");
    st.reset(byId(*doc, "f"));
    sub = submitForm(byId(*doc, "f"), nullptr, st, "http://h/");
    EXPECT_EQ(sub->url, "http://h/x?q=a&r=1&s=a");
}

TEST(Forms, PostEncodings) {
    const auto doc = parseHtml(R"H(<form id=f method=post action=submit><input name=a value="1 2"><input name='b"c' value=x></form>
        <form id=m method=POST enctype=multipart/form-data action=up><input name=a value=v></form>
        <form id=t method=post enctype=text/plain><input name=a value=v></form>
        <form id=d method=dialog><input name=a></form>
        <form id=j action="javascript:alert(1)"><input name=a></form>)H");
    FormState st;
    auto sub = submitForm(byId(*doc, "f"), nullptr, st, "https://h/dir/page");
    ASSERT_TRUE(sub);
    EXPECT_TRUE(sub->post);
    EXPECT_EQ(sub->url, "https://h/dir/submit");
    EXPECT_EQ(sub->content_type, "application/x-www-form-urlencoded");
    EXPECT_EQ(sub->body, "a=1+2&b%22c=x");
    sub = submitForm(byId(*doc, "m"), nullptr, st, "https://h/");
    ASSERT_TRUE(sub);
    ASSERT_TRUE(sub->content_type.starts_with("multipart/form-data; boundary="));
    const std::string boundary = sub->content_type.substr(sub->content_type.find('=') + 1);
    EXPECT_EQ(sub->body, "--" + boundary + "\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nv\r\n--" + boundary + "--\r\n");
    sub = submitForm(byId(*doc, "t"), nullptr, st, "https://h/p");
    ASSERT_TRUE(sub);
    EXPECT_EQ(sub->body, "a=v\r\n");
    EXPECT_FALSE(submitForm(byId(*doc, "d"), nullptr, st, "https://h/"));
    EXPECT_FALSE(submitForm(byId(*doc, "j"), nullptr, st, "https://h/"));
}

TEST(Forms, SubmitterOverridesAndOwners) {
    const auto doc = parseHtml(R"H(<form id=f action=/a><button id=b formaction=/b formmethod=post name=x value=1>B</button></form>
        <input id=out form=f name=o value=outside>)H");
    FormState st;
    const auto sub = submitForm(byId(*doc, "f"), byId(*doc, "b"), st, "http://h/");
    ASSERT_TRUE(sub);
    EXPECT_TRUE(sub->post);
    EXPECT_EQ(sub->url, "http://h/b");
    EXPECT_EQ(sub->body, "x=1&o=outside");
    EXPECT_EQ(formOwner(byId(*doc, "out")), byId(*doc, "f"));
    EXPECT_EQ(defaultButton(byId(*doc, "f")), byId(*doc, "b"));
}

TEST(Forms, ControlsAndLabels) {
    const auto doc = parseHtml(R"H(<label id=l1>Name <input id=i1></label><label id=l2 for=i2>Mail</label><input id=i2 type=email>
        <select id=s><optgroup><option disabled>a<option>b</optgroup></select><input id=m maxlength=3>)H");
    EXPECT_EQ(labelledControl(byId(*doc, "l1")), byId(*doc, "i1"));
    EXPECT_EQ(labelledControl(byId(*doc, "l2")), byId(*doc, "i2"));
    EXPECT_EQ(controlKind(byId(*doc, "i2")), ControlKind::Text);
    FormState st;
    EXPECT_EQ(st.selected(byId(*doc, "s")), 1);   // the first that isn't disabled
    st.setValue(byId(*doc, "m"), "한글입니다");
    EXPECT_EQ(st.value(byId(*doc, "m")), "한글입");   // maxlength counts characters
}

TEST(Forms, ImplicitSubmission) {
    const auto doc = parseHtml(R"H(<form id=one><input name=a></form><form id=two><input name=a><input name=b></form>)H");
    EXPECT_TRUE(implicitSubmissionAllowed(byId(*doc, "one")));
    EXPECT_FALSE(implicitSubmissionAllowed(byId(*doc, "two")));
}

TEST(Forms, HostileForms) {
    // Many controls, enormous values, groups and forms that point at each other.
    std::string html = "<form id=f action=/x>";
    for (int i = 0; i < 20000; ++i) html += "<input type=radio name=r value=" + std::to_string(i) + ">";
    html += "<select name=s>";
    for (int i = 0; i < 20000; ++i) html += "<option>o";
    html += "</select></form><input form=f name=far value=1><form id=g action='http://[::1'><input name=a></form>";
    const auto doc = parseHtml(html);
    FormState st;
    std::vector<const Node*> radios;
    std::vector<const Node*> todo{doc->root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (controlKind(n) == ControlKind::Radio && radios.size() < 3) radios.push_back(n);
        todo.insert(todo.end(), n->children.begin(), n->children.end());
    }
    ASSERT_EQ(radios.size(), 3u);
    st.activate(radios[0]);
    st.activate(radios[1]);
    EXPECT_FALSE(st.checked(radios[0]));
    st.setValue(radios[2], std::string(8 << 20, 'x'));
    EXPECT_LE(st.value(radios[2]).size(), kMaxFieldBytes);
    const auto sub = submitForm(byId(*doc, "f"), nullptr, st, "http://h/");
    ASSERT_TRUE(sub);
    EXPECT_LT(sub->url.size(), 64u << 20);
    (void)submitForm(byId(*doc, "g"), nullptr, st, "http://h/");
    (void)submitForm(nullptr, nullptr, st, "");
}

TEST(Forms, AnEucKrPageSubmitsInEucKr) {
    // yes24's search: the page is EUC-KR, so is its query. What EUC-KR lacks goes as &#n;.
    auto doc = parseHtml("<form id=f action=/s><input name=q value='\xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x98\x80'>"
                         "<input type=hidden name=_charset_></form>"
                         "<form id=g action=/s accept-charset=utf-8><input name=q value='\xED\x95\x9C'></form>");
    FormState st;
    auto sub = submitForm(byId(*doc, "f"), nullptr, st, "http://h/", "ks_c_5601-1987");
    ASSERT_TRUE(sub);
    EXPECT_EQ(sub->url, "http://h/s?q=%C7%D1%B1%DB+%26%23128512%3B&_charset_=EUC-KR");
    sub = submitForm(byId(*doc, "g"), nullptr, st, "http://h/", "euc-kr");
    ASSERT_TRUE(sub);
    EXPECT_EQ(sub->url, "http://h/s?q=%ED%95%9C");
}
