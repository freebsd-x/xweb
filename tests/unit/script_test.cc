// A page's scripts against its tree: the DOM, events, timers and forms, as pages use them.

#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "xweb/dom.h"
#include "xweb/forms.h"
#include "xweb/html_parser.h"
#include "xweb/script.h"

using namespace fbx::xweb;

namespace {

struct Page {
    std::unique_ptr<Document> doc;
    FormState forms;
    std::string console;
    std::string navigated;
    std::vector<const Node*> activated, submitted;
    std::unique_ptr<PageScript> script;

    explicit Page(const std::string& html, const std::string& external = "") {
        doc = parseHtml(html);
        PageScript::Host host;
        host.url = "https://example.org/dir/page.html";
        host.base = host.url;
        host.forms = &forms;
        host.console = [this](std::string_view level, const std::string& text) { console += std::string(level) + ":" + text + "\n"; };
        host.navigate = [this](const std::string& url) { navigated = url; };
        host.rect = [](const Node*) { return std::optional<ElementRect>(ElementRect{10, 20, 300, 40}); };
        host.activate = [this](const Node* n) { activated.push_back(n); };
        host.submit = [this](const Node* n) { submitted.push_back(n); };
        script = std::make_unique<PageScript>(*doc, host);
        script->run([external](const std::string& url) -> std::optional<std::string> {
            if (url == "https://example.org/dir/lib.js") return external;
            return std::nullopt;
        });
    }

    Node* byId(const std::string& id) {
        std::vector<Node*> todo{doc->root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isElement())
                if (const std::string* v = n->attr("id"); v && *v == id) return n;
            todo.insert(todo.end(), n->children.begin(), n->children.end());
        }
        return nullptr;
    }

    std::string body() { return serializeChildren(*doc->body()); }
};

}  // namespace

TEST(Script, ChangesTheTree) {
    Page p(R"(<body><ul id=list><li>one</li></ul><p id=t class="a b">x</p>
        <script>
          const ul = document.getElementById('list');
          const li = document.createElement('li');
          li.textContent = 'two';
          ul.appendChild(li);
          ul.insertAdjacentHTML('beforeend', '<li class=n>three</li>');
          document.querySelector('#t').classList.add('c');
          document.querySelector('#t').classList.remove('a');
          document.querySelector('p').style.color = 'red';
          document.querySelector('p').setAttribute('data-k', 'v');
          document.querySelectorAll('li').forEach((e, i) => e.dataset && e.setAttribute('n', i));
          document.title = 'Changed';
        </script>)");
    EXPECT_EQ(p.console, "");
    const std::string b = p.body();
    EXPECT_NE(b.find(R"(<li n="1">two</li>)"), std::string::npos) << b;
    EXPECT_NE(b.find(R"(<li class="n" n="2">three</li>)"), std::string::npos) << b;
    EXPECT_NE(b.find(R"(class="b c")"), std::string::npos) << b;
    EXPECT_NE(b.find(R"(style="color: red;")"), std::string::npos) << b;
    EXPECT_TRUE(p.script->takeDirty());
}

TEST(Script, ReadsTheTree) {
    Page p(R"(<head><title> My  page </title></head><body><div id=a><span>x</span><b>y</b>text</div><a id=l href="../up.html">l</a>
        <script>
          const a = document.getElementById('a');
          console.log([a.children.length, a.childNodes.length, a.firstElementChild.tagName, a.lastChild.nodeType,
                       a.textContent, document.title, document.getElementById('l').href, a.innerHTML,
                       a.matches('div#a'), a.querySelector('b').closest('#a') === a, document.body.contains(a),
                       a.getBoundingClientRect().width, document.readyState, location.hostname, location.pathname].join('|'));
        </script>)");
    EXPECT_EQ(p.console, "log:2|3|SPAN|3|xytext|My page|https://example.org/up.html|<span>x</span><b>y</b>text|true|true|true|300|loading|example.org|/dir/page.html\n");
}

TEST(Script, EventsDispatchAndCancel) {
    Page p(R"(<body><div id=outer><button id=b>go</button></div><a id=l href=/x onclick="return false">x</a>
        <script>
          const log = [];
          document.getElementById('outer').addEventListener('click', e => log.push('outer-capture'), true);
          document.getElementById('outer').addEventListener('click', e => log.push('outer-bubble:' + e.target.id));
          document.getElementById('b').addEventListener('click', e => { log.push('button'); e.preventDefault(); });
          document.addEventListener('click', e => log.push('document'));
          window.addEventListener('DOMContentLoaded', () => log.push('ready'));
          window.onload = () => console.log(log.join(','));
          window.report = () => console.log(log.join(','));
        </script>)");
    EXPECT_EQ(p.console, "log:ready\n");
    EXPECT_FALSE(p.script->click(p.byId("b"), 5, 5));   // cancelled
    EXPECT_FALSE(p.script->click(p.byId("l"), 5, 5));   // onclick="return false"
    Page q(R"(<body><div id=d></div><script>
        document.getElementById('d').addEventListener('click', e => { e.stopPropagation(); console.log('d'); });
        document.body.addEventListener('click', () => console.log('body'));
        </script>)");
    EXPECT_TRUE(q.script->click(q.byId("d"), 1, 1));
    EXPECT_EQ(q.console, "log:d\n");
}

TEST(Script, TimersRunInOrder) {
    Page p(R"(<body><script>
        const out = [];
        setTimeout(() => out.push('b'), 20);
        setTimeout(() => out.push('a'), 0);
        const id = setInterval(() => { out.push('i'); if (out.filter(x => x == 'i').length == 2) clearInterval(id); }, 5);
        requestAnimationFrame(t => out.push('raf'));
        queueMicrotask(() => out.push('micro'));
        setTimeout(() => console.log(out.join(',')), 60);
        </script>)");
    for (int i = 0; i < 40 && p.script->busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p.script->tick();
    }
    EXPECT_FALSE(p.script->busy());
    // Microtasks first, the 0ms timer before the 5ms ones, the 20ms one last; the frame
    // somewhere in between, as its timing has it.
    std::string order = p.console;
    const size_t raf = order.find("raf,");
    ASSERT_NE(raf, std::string::npos) << order;
    order.erase(raf, 4);
    EXPECT_EQ(order, "log:micro,a,i,i,b\n");
}

TEST(Script, FormsAndExternalScripts) {
    Page p(R"(<body><form id=f action=/s><input id=q name=q value=start><input type=checkbox id=c>
        <select id=s><option value=1>one<option value=2>two</select></form>
        <script src=lib.js></script>
        <script>
          const q = document.getElementById('q');
          console.log(q.value, typeof fromLib);
          q.value = 'typed';
          document.getElementById('c').checked = true;
          document.getElementById('s').value = '2';
          document.getElementById('f').addEventListener('submit', e => { e.preventDefault(); console.log('stopped'); });
        </script>)", "var fromLib = 1;");
    EXPECT_EQ(p.console, "log:start number\n");
    EXPECT_EQ(p.forms.value(p.byId("q")), "typed");
    EXPECT_TRUE(p.forms.checked(p.byId("c")));
    EXPECT_EQ(p.forms.selected(p.byId("s")), 1);
    EXPECT_FALSE(p.script->submit(p.byId("f"), nullptr));
    EXPECT_NE(p.console.find("stopped"), std::string::npos);
}

TEST(Script, ErrorsAreReportedNotFatal) {
    Page p(R"(<body><script>undefinedFunction();</script><script>console.log('second ran'); location.href = 'next.html';</script>
        <script src=missing.js></script><script type=text/template>not code</script>)");
    EXPECT_NE(p.console.find("error:ReferenceError: undefinedFunction is not defined"), std::string::npos) << p.console;
    EXPECT_NE(p.console.find("log:second ran"), std::string::npos);
    EXPECT_EQ(p.navigated, "https://example.org/dir/next.html");
}

TEST(Script, InsertedScriptsRunAndRunawayIsStopped) {
    Page p(R"(<body><script>
        window.addEventListener('load', () => {
          const s = document.createElement('script');
          s.textContent = 'console.log("inserted ran")';
          document.body.appendChild(s);
        });
        </script>)");
    EXPECT_NE(p.console.find("log:inserted ran"), std::string::npos) << p.console;
    Page q(R"(<body><script>while (true) {}</script><script>console.log('after')</script>)");
    EXPECT_NE(q.console.find("Stopped: the script ran too long"), std::string::npos) << q.console;
    EXPECT_NE(q.console.find("log:after"), std::string::npos);
}
