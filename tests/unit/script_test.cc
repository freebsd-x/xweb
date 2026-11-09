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
    std::vector<std::string> fetched;
    std::vector<std::function<void()>> pending;   // network answers, delivered by deliver()
    void deliver() {
        while (!pending.empty()) {
            auto todo = std::move(pending);
            pending.clear();
            for (auto& f : todo) f();
        }
    }
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
        // A network that answers at once, from a table; what was asked is kept.
        host.fetch = [this](const PageScript::Host::Request& r, std::function<void(PageScript::Host::Response)> done) {
            std::string hs;
            for (const auto& [k, v] : r.headers) hs += k + "=" + v + ";";
            fetched.push_back(r.method + " " + r.url + " " + r.content_type + " " + r.body + " [" + hs + "] cookies=" + (r.cookies ? "yes" : "no"));
            PageScript::Host::Response res;
            const auto answer = [&](int status, std::string type, std::string body, std::vector<std::pair<std::string, std::string>> headers = {}) {
                res.ok = true;
                res.status = status;
                res.url = r.url;
                res.content_type = type;
                res.body = std::move(body);
                headers.emplace_back("content-type", res.content_type);
                res.headers = std::move(headers);
            };
            if (r.url == "https://example.org/dir/data.json") answer(200, "application/json", R"({"items":[1,2,3],"name":"x"})", {{"x-secret", "s"}});
            else if (r.url == "https://example.org/api") answer(201, "text/plain", "made " + r.body);
            else if (r.url == "https://example.org/missing") answer(404, "text/html", "nope");
            else if (r.url == "https://api.other.test/open") answer(200, "text/plain", "open data", {{"access-control-allow-origin", "*"}, {"x-shown", "1"}, {"x-hidden", "2"}, {"access-control-expose-headers", "x-shown"}});
            else if (r.url == "https://api.other.test/closed") answer(200, "text/plain", "private");
            else if (r.url == "https://api.other.test/rw" && r.method == "OPTIONS")
                answer(204, "", "", {{"access-control-allow-origin", "https://example.org"}, {"access-control-allow-methods", "PUT"}, {"access-control-allow-headers", "x-token"}});
            else if (r.url == "https://api.other.test/rw") answer(200, "text/plain", "put ok", {{"access-control-allow-origin", "https://example.org"}});
            else res.error = "unreachable";
            pending.push_back([done, res] { done(res); });
        };
        script = std::make_unique<PageScript>(*doc, host);
        script->run([external](const std::string& url) -> std::optional<std::string> {
            if (url == "https://example.org/dir/lib.js") return external;
            if (url == "https://example.org/dir/mod.js") return std::string("export const word = 'module'; export default (s) => s.toUpperCase();");
            if (url == "https://example.org/vendor/dep.js") return std::string("export const dep = 'mapped';");
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

TEST(Script, ModulesPromisesAndThePrelude) {
    Page p(R"(<body><div id=out></div>
        <script type=importmap>{"imports": {"dep": "/vendor/dep.js"}}</script>
        <script type=module>
          import shout, { word } from './mod.js';
          import { dep } from 'dep';
          const parts = await Promise.all([shout(word), Promise.resolve(dep)]);
          document.getElementById('out').textContent = parts.join(' ') + ' ' + (document.currentScript === null);
        </script>
        <script nomodule>console.log('WRONG: nomodule ran')</script>
        <script>
          console.log('classic first');
          const u = new URL('../x/y?a=1#h', 'https://example.org/dir/page.html');
          u.searchParams.append('b', '2');
          console.log(u.href, u.origin);
          const c = new AbortController();
          c.signal.addEventListener('abort', () => console.log('aborted', c.signal.aborted, c.signal.reason.name));
          c.abort();
          const w = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT);
          const tags = []; while (w.nextNode()) tags.push(w.currentNode.tagName);
          console.log(tags.join(','));
          (async () => { await null; console.log('microtask after the script'); })();
          console.log('script end');
        </script>)");
    EXPECT_EQ(p.byId("out")->textContent(), "MODULE mapped true") << p.console;
    EXPECT_EQ(p.console.find("WRONG"), std::string::npos) << p.console;
    EXPECT_NE(p.console.find("log:classic first\nlog:https://example.org/x/y?a=1&b=2#h https://example.org\nlog:aborted true AbortError\n"
                             "log:DIV,SCRIPT,SCRIPT,SCRIPT,SCRIPT\nlog:script end\nlog:microtask after the script\n"), std::string::npos) << p.console;
}

TEST(Script, FetchAndXMLHttpRequest) {
    Page p(R"(<body><script>
        fetch('data.json').then(r => { console.log('status', r.status, r.ok, r.headers.get('Content-Type')); return r.json(); })
          .then(j => console.log('json', j.items.length, j.name));
        fetch('/api', {method: 'POST', body: JSON.stringify({a: 1}), headers: {'Content-Type': 'application/json'}})
          .then(r => r.text()).then(t => console.log('posted', t));
        fetch('/missing').then(r => console.log('missing', r.status, r.ok));
        fetch('https://elsewhere.test/').catch(e => console.log('failed', e.name, e.message));
        const c = new AbortController(); fetch('data.json', {signal: c.signal}).catch(e => console.log('aborted', e.name)); c.abort();
        const x = new XMLHttpRequest();
        x.onload = () => console.log('xhr', x.status, x.response.name, x.readyState);
        x.responseType = 'json';
        x.open('GET', 'data.json'); x.send();
        new Response('{"z":9}').json().then(j => console.log('response', j.z));
        new Blob(['ab', new Uint8Array([99])]).text().then(t => console.log('blob', t));
        </script>)");
    p.deliver();
    p.script->tick();
    for (const char* want : {"log:status 200 true application/json\n", "log:json 3 x\n", "log:posted made {\"a\":1}\n", "log:missing 404 false\n",
                             "log:failed TypeError Failed to fetch: unreachable\n", "log:aborted AbortError\n", "log:xhr 200 x 4\n",
                             "log:response 9\n", "log:blob abc\n"})
        EXPECT_NE(p.console.find(want), std::string::npos) << want << "\n" << p.console;
    EXPECT_NE(std::find_if(p.fetched.begin(), p.fetched.end(), [](const std::string& f) { return f.starts_with("POST https://example.org/api application/json {\"a\":1} [content-type=application/json;Origin=https://example.org;Referer=https://example.org/dir/page.html;] cookies=yes"); }), p.fetched.end()) << p.fetched[1];
}

TEST(Script, CustomElementsShadowRootsAndObservers) {
    Page p(R"(<body><my-early id=early title=t1></my-early><div id=host></div>
        <section id=dsd><template shadowrootmode=open><p id=inside>declarative</p><slot></slot></template><b>light</b></section>
        <script>
          const log = [];
          class MyEarly extends HTMLElement {
            static observedAttributes = ['title'];
            constructor() { super(); log.push('ctor:' + this.id); }
            connectedCallback() { log.push('connected:' + this.id); }
            disconnectedCallback() { log.push('disconnected:' + this.id); }
            attributeChangedCallback(n, o, v) { log.push('attr:' + n + ':' + o + '>' + v); }
          }
          console.log('before', document.getElementById('early') instanceof MyEarly, document.querySelectorAll(':defined').length > 0);
          customElements.whenDefined('my-early').then(c => log.push('whenDefined:' + (c === MyEarly)));
          customElements.define('my-early', MyEarly);
          const early = document.getElementById('early');
          early.title = 't2';
          early.remove();
          document.body.append(early);
          const made = document.createElement('my-early');
          made.id = 'made';
          document.body.appendChild(made);
          console.log(log.join(' '));
          console.log('upgraded', early instanceof MyEarly, early instanceof HTMLElement, made instanceof MyEarly, customElements.get('my-early') === MyEarly);
          try { customElements.define('nodash', class extends HTMLElement {}); } catch (e) { console.log('bad name', e.name); }
          try { new HTMLDivElement(); } catch (e) { console.log('illegal', e.name); }

          const host = document.getElementById('host');
          const root = host.attachShadow({mode: 'open'});
          root.innerHTML = '<span id=s>shadow</span><slot name=x></slot>';
          console.log('shadow', host.shadowRoot === root, root.host === host, root.mode, root.getElementById('s').textContent,
                      document.getElementById('s'), root.querySelector('span').id, root instanceof ShadowRoot, root instanceof DocumentFragment);
          const light = document.createElement('i'); light.slot = 'x'; host.append(light);
          console.log('slot', root.querySelector('slot').assignedNodes().length, light.assignedSlot === root.querySelector('slot'),
                      root.querySelector('span').getRootNode() === root);
          const dsd = document.getElementById('dsd');
          console.log('declarative', dsd.shadowRoot.getElementById('inside').textContent, dsd.shadowRoot.querySelector('slot').assignedElements()[0].tagName);
          try { host.attachShadow({mode: 'open'}); } catch (e) { console.log('twice', e.name); }

          const seen = [];
          const mo = new MutationObserver((records, o) => {
            for (const r of records) seen.push(r.type + ':' + (r.attributeName || '') + ':' + (r.oldValue ?? '') + ':' + r.addedNodes.length + '/' + r.removedNodes.length);
            console.log('observed', seen.join(' '), o === mo);
          });
          mo.observe(document.body, {childList: true, subtree: true, attributes: true, attributeOldValue: true, attributeFilter: ['data-x']});
          const d = document.createElement('div');
          document.body.append(d);
          d.setAttribute('data-x', '1');
          d.setAttribute('data-x', '2');
          d.setAttribute('other', '3');
          d.remove();
          console.log('sync end', seen.length);

          console.log(document.createElement('dialog') instanceof HTMLDialogElement, document.createElement('img') instanceof HTMLImageElement,
                      document.createElement('video') instanceof HTMLMediaElement, document.createElement('foo') instanceof HTMLUnknownElement,
                      document.createElement('article').constructor === HTMLElement, Object.prototype.toString.call(new DocumentFragment()));
          const dlg = document.createElement('dialog'); dlg.showModal(); const was = dlg.open; dlg.close('ok');
          console.log('dialog', was, dlg.open, dlg.returnValue);
          const tpl = document.createElement('template'); tpl.innerHTML = '<b>x</b>';
          console.log('template', tpl.content.firstChild.tagName, tpl.childNodes.length);
          const a = document.createElement('a'); a.href = 'https://h.test:8080/p/q?z=1#f';
          console.log('link', a.host, a.pathname, a.search, a.hash);
          const sc = structuredClone({m: new Map([[1, {d: new Date(0)}]]), arr: [1, [2]], self: null});
          console.log('clone', sc.m.get(1).d.getTime(), sc.arr[1][0]);
          const sheet = new CSSStyleSheet(); sheet.replaceSync('span { color: red }'); root.adoptedStyleSheets = [sheet];
          console.log('adopted', root.adoptedStyleSheets.length);
        </script>)");
    p.script->tick();
    for (const char* want : {
             "log:before false true\n",
             "log:ctor:early attr:title:null>t1 connected:early attr:title:t1>t2 disconnected:early connected:early ctor: connected:made\n",
             "log:upgraded true true true true\n", "log:bad name SyntaxError\n", "log:illegal TypeError\n",
             "log:shadow true true open shadow null s true true\n", "log:slot 1 true true\n", "log:declarative declarative B\n",
             "log:twice NotSupportedError\n", "log:sync end 0\n",
             "log:observed childList:::1/0 attributes:data-x::0/0 attributes:data-x:1:0/0 childList:::0/1 true\n",
             "log:true true true true true [object DocumentFragment]\n", "log:dialog true false ok\n", "log:template B 0\n",
             "log:link h.test:8080 /p/q ?z=1 #f\n", "log:clone 0 2\n", "log:adopted 1\n"})
        EXPECT_NE(p.console.find(want), std::string::npos) << want << "\n" << p.console;
    EXPECT_EQ(p.console.find("error"), std::string::npos) << p.console;
}

TEST(Script, CryptoAndIntl) {
    Page p(R"(<body><script>
        const a = crypto.getRandomValues(new Uint8Array(32));
        console.log('random', a.some(x => x !== 0), /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(crypto.randomUUID()));
        const d = Date.UTC(2026, 10, 8, 15, 4, 5);
        console.log(new Intl.NumberFormat('en-US').format(1234567.891), new Intl.NumberFormat('en', {style: 'currency', currency: 'USD'}).format(-3.5),
                    new Intl.NumberFormat('en', {notation: 'compact'}).format(12500), new Intl.NumberFormat('en', {style: 'percent'}).format(0.256));
        console.log(new Intl.DateTimeFormat('en-US', {timeZone: 'UTC'}).format(d), '|',
                    new Intl.DateTimeFormat('en', {timeZone: 'UTC', dateStyle: 'medium'}).format(d), '|',
                    new Intl.DateTimeFormat('en', {timeZone: 'UTC', month: 'long', day: 'numeric', year: 'numeric', hour: 'numeric', minute: '2-digit'}).format(d));
        const rtf = new Intl.RelativeTimeFormat('en', {numeric: 'auto'});
        console.log(rtf.format(-1, 'day'), '|', rtf.format(3, 'hours'), '|', rtf.format(-2, 'month'), '|', new Intl.ListFormat('en').format(['a', 'b', 'c']),
                    '|', new Intl.PluralRules('en', {type: 'ordinal'}).select(22), ['b', 'a', 'C'].sort(new Intl.Collator('en').compare).join(''));
        </script>)");
    for (const char* want : {"log:random true true\n", "log:1,234,567.891 -$3.50 12.5K 26%\n",
                             "log:11/8/2026 | Nov 8, 2026 | November 8, 2026, 3:04 PM\n", "log:yesterday | in 3 hours | 2 months ago | a, b, and c | two abC\n"})
        EXPECT_NE(p.console.find(want), std::string::npos) << want << "\n" << p.console;
}

TEST(Script, CrossOriginRequestsFollowCors) {
    Page p(R"(<body><script>
        const log = (...a) => console.log(...a);
        fetch('https://api.other.test/open').then(async r => log('open', r.type, await r.text(), r.headers.get('x-shown'), r.headers.get('x-hidden')));
        fetch('https://api.other.test/closed').then(r => log('closed read!'), e => log('closed', e.name));
        fetch('https://api.other.test/rw', {method: 'PUT', headers: {'X-Token': 't'}, body: 'b'}).then(async r => log('rw', await r.text()));
        fetch('https://api.other.test/open', {credentials: 'include'}).then(r => log('star with credentials read!'), e => log('star with credentials', e.name));
        fetch('https://api.other.test/closed', {mode: 'no-cors'}).then(async r => log('opaque', r.type, r.status, (await r.text()).length));
        fetch('data.json').then(r => log('same', r.type, r.headers.get('x-secret')));
        fetch('https://api.other.test/closed', {mode: 'same-origin'}).catch(e => log('same-origin mode', e.name));
        </script>)");
    p.deliver();
    p.deliver();
    for (const char* want : {"log:open cors open data 1 null\n", "log:closed TypeError\n", "log:rw put ok\n", "log:star with credentials TypeError\n",
                             "log:opaque opaque 0 0\n", "log:same basic s\n", "log:same-origin mode TypeError\n"})
        EXPECT_NE(p.console.find(want), std::string::npos) << want << "\n" << p.console;
    const auto sent = [&](const std::string& prefix) {
        return std::find_if(p.fetched.begin(), p.fetched.end(), [&](const std::string& f) { return f.starts_with(prefix); }) != p.fetched.end();
    };
    EXPECT_TRUE(sent("OPTIONS https://api.other.test/rw   [Origin=https://example.org;Access-Control-Request-Method=PUT;Access-Control-Request-Headers=x-token;] cookies=no"));
    EXPECT_TRUE(sent("GET https://api.other.test/open   [Origin=https://example.org;Referer=https://example.org/;] cookies=no"));
    EXPECT_TRUE(sent("GET https://api.other.test/open   [Origin=https://example.org;Referer=https://example.org/;] cookies=yes"));   // credentials: include
    EXPECT_TRUE(sent("GET https://example.org/dir/data.json   [Referer=https://example.org/dir/page.html;] cookies=yes"));
}

TEST(Script, DetailsToggleTellsThePage) {
    Page p(R"(<body><details id=d><summary>s</summary>x</details><script>
        const d = document.getElementById('d');
        d.addEventListener('toggle', () => console.log('toggle', d.open));
        </script>)");
    p.script->toggle(p.byId("d"));
    EXPECT_TRUE(p.byId("d")->attr("open"));
    p.script->toggle(p.byId("d"));
    EXPECT_FALSE(p.byId("d")->attr("open"));
    EXPECT_EQ(p.console, "log:toggle true\nlog:toggle false\n");
}

TEST(Script, SelectsAndOptionsAsPagesBuildThem) {
    Page p(R"(<body><select id=s><option>a</option></select><script>
        const log = (...a) => console.log(...a);
        const s = document.getElementById('s');
        const o = document.createElement('option');
        o.value = 'v2'; o.text = '  Two  '; o.selected = true;
        s.add(o);
        s.add(new Option('Zero', 'v0'), 0);
        log(s.length, s.options[0].text, s.options[2].value, s.options[2].text, s.selectedIndex, o.index, o.selected);
        s.selectedIndex = 0;
        log(o.selected, s.options[0].selected, s.value);
        s.remove(0);
        log(s.length, s.item(0).text, new Image(3, 4).getAttribute('width'), o instanceof HTMLOptionElement);
        </script>)");
    EXPECT_EQ(p.console, "log:3 Zero v2 Two 2 2 true\nlog:false true v0\nlog:2 a 3 true\n");
}
