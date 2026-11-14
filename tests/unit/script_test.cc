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
        // Canvas text in a pretend font: 5px a character, 8 above the baseline, 2 below, solid.
        host.canvas_text = [](const std::string& text, const std::string&, float scale, bool render) {
            PageScript::Host::CanvasText t;
            t.advance = 5.0f * static_cast<float>(text.size());
            t.ascent = 8;
            t.descent = 2;
            if (render) {
                t.mask.width = static_cast<uint32_t>(t.advance * scale);
                t.mask.height = static_cast<uint32_t>(10 * scale);
                t.mask.rgba.assign(static_cast<size_t>(t.mask.width) * t.mask.height * 4, 255);
            }
            return std::optional<PageScript::Host::CanvasText>(t);
        };
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
        // Scripts not fetched ahead, as a page's own fetch would bring them, a task later.
        host.fetch_script = [this](const std::string& url, const std::string&, std::function<void(std::optional<std::string>)> done) {
            std::optional<std::string> body;
            if (url == "https://example.org/dir/chunk.js") body = "console.log('chunk ran', typeof window.chunkFlag); window.chunkLoaded = true;";
            pending.push_back([done, body] { done(body); });
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

TEST(Script, DocumentsOfAScriptsOwn) {
    Page p(R"(<body><p id=x>page</p><script>
        const log = (...a) => console.log(...a);
        const d = document.implementation.createHTMLDocument('');
        d.body.innerHTML = '<form></form><form></form>';   // what jQuery checks
        log(d.body.childNodes.length, d.title === '', d.documentElement.localName, d.head !== document.head);
        log(document.body.querySelectorAll('form').length, document.getElementById('x').textContent);
        const t = document.implementation.createHTMLDocument('T');
        t.body.innerHTML = '<span id=x>theirs</span>';
        log(t.title, t.getElementById('x').textContent, document.getElementById('x').textContent);
        log(typeof document.implementation.hasFeature, t instanceof Document);
        </script>)");
    EXPECT_EQ(p.console, "log:2 true html true\nlog:0 page\nlog:T theirs page\nlog:function true\n");
}

TEST(Script, StyleSheetsOfElements) {
    // What styled-components does: an empty <style>, its sheet found through
    // document.styleSheets, rules inserted.
    Page p(R"(<head><style id=s></style></head><body><script>
        const log = (...a) => console.log(...a);
        const el = document.getElementById('s');
        const sheet = el.sheet;
        log(sheet === el.sheet, document.styleSheets.length, document.styleSheets[0] === sheet, sheet.ownerNode === el);
        log(sheet.insertRule('.a{color:red}', 0), sheet.insertRule('.b{color:blue}', 1), sheet.cssRules.length, sheet.cssRules[1].cssText);
        sheet.deleteRule(0);
        log(sheet.cssRules.length, sheet.cssRules[0].cssText);
        try { sheet.insertRule('.c{}', 5); } catch (e) { log(e.message); }
        </script>)");
    EXPECT_EQ(p.console, "log:true 1 true true\nlog:0 1 2 .b{color:blue}\nlog:1 .b{color:blue}\nlog:IndexSizeError: no such rule index\n");
    ASSERT_EQ(p.byId("s")->adopted_css.size(), 1u);
}

TEST(Script, InsertedScriptsAreFetchedAndRun) {
    // What bundlers do to load a chunk: a <script src> added later, waited on by load.
    Page p(R"(<body><script>
        const log = (...a) => console.log(...a);
        window.chunkFlag = 1;
        const s = document.createElement('script');
        s.src = 'chunk.js';
        s.onload = () => log('load', window.chunkLoaded);
        document.head.appendChild(s);
        const bad = document.createElement('script');
        bad.src = 'missing.js';
        bad.onerror = () => log('error event');
        document.head.appendChild(bad);
        log('inserted');
        </script>)");
    EXPECT_EQ(p.console, "log:inserted\n");
    p.deliver();
    EXPECT_EQ(p.console, "log:inserted\nlog:chunk ran number\nlog:load true\nlog:error event\n");
}

TEST(Script, NodesCompareByWhatTheyAre) {
    Page p(R"(<body><div id=a class=x title=t><b>hi</b></div><div title=t class=x><b>hi</b></div><div class=x><b>ho</b></div><script>
        const [a, b, c] = document.querySelectorAll('body > div');
        console.log(a.isEqualNode(b), a.isEqualNode(c), a.isEqualNode(null), a.isSameNode(a), a.isSameNode(b), a.cloneNode(true).isEqualNode(a));
        </script>)");
    EXPECT_EQ(p.console, "log:false false false true false true\n");
}

TEST(Script, UnhandledRejectionsReachThePage) {
    Page p(R"(<body><script>
        console.log(typeof PromiseRejectionEvent, new PromiseRejectionEvent('x', {reason: 1}).reason);
        addEventListener('unhandledrejection', e => { console.log('saw', e.reason, e.promise instanceof Promise); if (e.reason === 'quiet') e.preventDefault(); });
        Promise.reject('quiet');
        Promise.reject('loud');
        </script>)");
    EXPECT_NE(p.console.find("log:function 1\n"), std::string::npos) << p.console;
    EXPECT_NE(p.console.find("log:saw quiet true\n"), std::string::npos) << p.console;
    EXPECT_NE(p.console.find("log:saw loud true\n"), std::string::npos) << p.console;
    EXPECT_EQ(p.console.find("Uncaught (in promise) quiet"), std::string::npos) << p.console;
    EXPECT_NE(p.console.find("Uncaught (in promise) loud"), std::string::npos) << p.console;
}

TEST(Script, ClassListIsAList) {
    Page p(R"(<body class="a b  c"><script>
        const cl = document.body.classList;
        console.log(cl.length, cl[0], cl[2], cl.item(1), Array.from(cl).join('+'), [...cl].length, cl.value);
        </script>)");
    EXPECT_EQ(p.console, "log:3 a c b a+b+c 3 a b  c\n");
}

TEST(Script, StylePropertiesTakeWhatPagesAssign) {
    Page p(R"(<body><div id=d></div><script>
        const s = document.getElementById('d').style;
        s.opacity = 0; s.zIndex = 1; s.cssFloat = 'left'; s.marginRight = '-100%'; s.display = 'block';
        s['width'] = '100%'; s.setProperty('position', 'relative');
        console.log(document.getElementById('d').getAttribute('style'));
        console.log(s.opacity, s.zIndex, s.cssFloat, s.float, s.marginRight);
        </script>)");
    EXPECT_EQ(p.console, "log:opacity: 0; z-index: 1; float: left; margin-right: -100%; display: block; width: 100%; position: relative;\n"
                         "log:0 1 left left -100%\n");
}

TEST(Script, BlobUrls) {
    Page p(R"(<body><script>
        const u = URL.createObjectURL(new Blob(['hello'], {type: 'text/plain'}));
        console.log(u.startsWith('blob:https://example.org/'), u.length > 40);
        fetch(u).then(r => r.text().then(t => console.log('got', t, r.headers.get('content-type'))));
        URL.revokeObjectURL(u);
        fetch(u).catch(e => console.log('revoked', e.name));
        </script>)");
    p.deliver();
    EXPECT_NE(p.console.find("log:true true\n"), std::string::npos) << p.console;
    EXPECT_NE(p.console.find("log:revoked TypeError\n"), std::string::npos) << p.console;
}

TEST(Script, MatchesOnEveryKindOfElement) {
    Page p(R"(<body><svg id=s><circle id=c /></svg><template id=t><p>x</p></template><script>
        const log = (...a) => console.log(...a);
        log(typeof document.body.matches, typeof document.getElementById('c').matches, typeof document.createElementNS('http://www.w3.org/2000/svg', 'g').matches);
        log(typeof document.getElementById('t').content, document.getElementById('t').content && document.getElementById('t').content.childNodes.length);
        log(typeof document.createElement('div').closest, typeof Element.prototype.webkitMatchesSelector, typeof Element.prototype.msMatchesSelector);
        const f = document.createRange().createContextualFragment('<b>1</b><i>2</i>');
        log(f.childNodes.length, f.firstChild.localName, document.createRange().collapsed);
        </script>)");
    EXPECT_EQ(p.console, "log:function function function\nlog:object 1\nlog:function function undefined\nlog:2 b true\n") << p.console;
}

TEST(Script, WindowLoadDoesntPassThroughTheDocument) {
    Page p(R"(<body><script>
        document.addEventListener('load', e => console.log('document saw load', e.target === document), true);
        window.addEventListener('load', e => console.log('window load', e.target === document));
        </script>)");
    EXPECT_EQ(p.console, "log:window load true\n") << p.console;
}

TEST(Script, IntersectionObserversSayWhatsInView) {
    // The fixture puts every element at (10, 20) 300x40, in an 1000x800 view.
    Page p(R"(<body><div id=a></div><script>
        const io = new IntersectionObserver(es => es.forEach(e => console.log('seen', e.target.id, e.isIntersecting)));
        io.observe(document.getElementById('a'));
        </script>)");
    for (int i = 0; i < 3; ++i) p.script->tick();
    EXPECT_NE(p.console.find("log:seen a true\n"), std::string::npos) << p.console;
}

TEST(Script, DOMParserMakesDocumentsOfItsOwn) {
    Page p(R"(<body><p id=x>page</p><script>
        const d = new DOMParser().parseFromString('<title>T</title><p id=x>parsed<script>console.log("ran!")<\/script>', 'text/html');
        console.log(d.title, d.getElementById('x').textContent, document.getElementById('x').textContent, d instanceof Document);
        const svg = new DOMParser().parseFromString('<svg xmlns="http://www.w3.org/2000/svg"><circle r="1"/></svg>', 'image/svg+xml');
        console.log(svg.querySelector('circle') !== null, new XMLSerializer().serializeToString(document.getElementById('x')));
        </script>)");
    EXPECT_EQ(p.console, "log:T parsedconsole.log(\"ran!\") page true\nlog:true <p id=\"x\">page</p>\n") << p.console;   // its script is text, not run
}

TEST(Script, FormMembersStayWithTheControls) {
    Page p(R"(<body><div id=d></div><form id=f><input id=i value=v><select id=s><option>a</select></form><script>
        const log = (...a) => console.log(...a);
        class XPicker extends HTMLElement { constructor() { super(); this.options = [1, 2]; this.type = 'fancy'; this.form = 'mine'; } }
        customElements.define('x-picker', XPicker);
        const x = document.createElement('x-picker');
        log(x.options.length, x.type, x.form, 'value' in document.getElementById('d'));
        log(document.getElementById('i').value, document.getElementById('i').form.id, document.getElementById('s').options.length, typeof document.getElementById('f').submit);
        const d = document.getElementById('d');
        d.insertAdjacentElement('beforeend', document.createElement('span'));
        d.insertAdjacentText('afterbegin', 'hi');
        log(d.innerHTML);
        </script>)");
    EXPECT_EQ(p.console, "log:2 fancy mine false\nlog:v f 1 function\nlog:hi<span></span>\n") << p.console;
}

TEST(Script, ListsAreNodeLists) {
    Page p(R"(<body><b></b><b></b><script>
        const l = document.querySelectorAll('b');
        if (!NodeList.prototype.myEach) NodeList.prototype.myEach = function (f) { for (let i = 0; i < this.length; i++) f(this[i]); };
        let n = 0; l.myEach(() => n++);
        console.log(l instanceof NodeList, document.body.children instanceof HTMLCollection, Array.isArray(l), n, l.item(1) === l[1]);
        </script>)");
    EXPECT_EQ(p.console, "log:true true true 2 true\n") << p.console;
}

TEST(Script, EveryCharacterDataInterfaceIsThere) {
    // The webcomponents polyfill patches all four without checking they exist.
    Page p(R"(<body><script>
        const r = ["Text", "Comment", "CDATASection", "ProcessingInstruction"].map(n => typeof window[n].prototype.__proto__);
        console.log(r.join(), Object.getPrototypeOf(CDATASection.prototype) === Text.prototype);
        </script>)");
    EXPECT_EQ(p.console, "log:object,object,object,object true\n") << p.console;
}

TEST(Script, ClonedTemplatesKeepTheirContents) {
    // The webcomponents loader's test: fail it and the page swaps in the whole polyfill.
    Page p(R"(<body><template id=t><i>x</i></template><script>
        const t = document.createElement('template'), t2 = document.createElement('template');
        t2.content.appendChild(document.createElement('div'));
        t.content.appendChild(t2);
        const c = t.cloneNode(true);
        const parsed = document.getElementById('t').cloneNode(true);
        console.log(c.content.childNodes.length, c.content.firstChild.content.childNodes.length,
                    t.cloneNode(false).content.childNodes.length, parsed.content.firstChild.localName);
        </script>)");
    EXPECT_EQ(p.console, "log:1 1 0 i\n") << p.console;
}

TEST(Script, CollectionsFindItemsByIdOrName) {
    Page p(R"(<body><form><input name=q><input id=go name=x></form><script>
        const els = document.forms[0].elements, kids = document.body.children;
        console.log(els.namedItem('q').name, els.namedItem('go').name, els.namedItem('x').id, els.namedItem('nope'),
                    kids.namedItem('go'), els.item(5), els.item(0) === els[0]);
        </script>)");
    EXPECT_EQ(p.console, "log:q x go null null null true\n") << p.console;
}

TEST(Script, TextNodesAreNotParents) {
    // Reddit's observer asks for querySelectorAll, then calls matches().
    Page p(R"(<body><p>hi</p><script>
        const t = document.querySelector('p').firstChild, f = document.createDocumentFragment();
        console.log(typeof t.querySelectorAll, typeof t.children, typeof f.querySelectorAll, typeof f.getElementsByTagName,
                    typeof document.querySelectorAll, typeof document.body.children, typeof t.remove);
        </script>)");
    EXPECT_EQ(p.console, "log:undefined undefined function undefined function object function\n") << p.console;
}

TEST(Script, AnimateFinishesAtOnceAndTabIndexIsSettable) {
    Page p(R"(<body><div id=d></div><button id=b></button><script>
        const d = document.getElementById('d');
        const a = d.animate([{ opacity: 0 }, { opacity: 0.5 }], { duration: 300, fill: 'forwards' });
        d.tabIndex = 3;
        console.log(a instanceof Animation, a.playState, d.style.opacity, d.getAttribute('tabindex'), d.tabIndex,
                    document.getElementById('b').tabIndex, typeof a.finished.then);
        </script>)");
    EXPECT_EQ(p.console, "log:true finished 0.5 3 3 0 function\n") << p.console;
}

TEST(Script, ConstraintValidation) {
    Page p(R"(<body><form><input id=a required><input id=e type=email value="x"><input id=n type=number min=2 value=1>
        <input id=p pattern="[a-z]+" value="abc"></form><script>
        const $ = (id) => document.getElementById(id);
        let invalid = 0;
        $('a').addEventListener('invalid', () => invalid++);
        const before = [$('a').validity.valueMissing, $('e').validity.typeMismatch, $('n').validity.rangeUnderflow, $('p').validity.valid];
        const formOk = document.forms[0].checkValidity();
        $('p').setCustomValidity('nope');
        class X extends HTMLElement { static formAssociated = true; constructor() { super(); this.i = this.attachInternals(); } }
        customElements.define('x-field', X);
        const x = new X();
        x.i.setValidity({ customError: true }, 'bad');
        console.log(before.join(), formOk, invalid, $('p').validity.customError, $('p').validationMessage,
                    $('a').validationMessage, x.i.validity.valid, x.i.validationMessage, typeof x.i.setFormValue);
        </script>)");
    EXPECT_EQ(p.console, "log:true,true,true,true false 1 true nope Please fill out this field. false bad function\n") << p.console;
}

TEST(Script, ConstructedSheetsListTheirRules) {
    Page p(R"(<body><script>
        const s = new CSSStyleSheet();
        s.replaceSync('/* c */ a { color: red } @media (min-width: 1px) { b { x: "}" } } @import url(x.css);');
        console.log(s.cssRules.length, s.cssRules[0].cssText, s.cssRules[2].cssText);
        </script>)");
    EXPECT_EQ(p.console, "log:3 a { color: red } @import url(x.css);\n") << p.console;
}

TEST(Script, OldPageApisYouTubeAndNasaUse) {
    Page p(R"(<body><button id=b>x</button><script>
        const e = document.createEvent('CustomEvent');
        e.initCustomEvent('hi', true, false, { n: 1 });
        let seen = '';
        document.body.addEventListener('hi', (ev) => { seen = ev.type + ev.detail.n + (window.event === ev); });
        document.body.dispatchEvent(e);
        const nav = performance.getEntriesByType('navigation')[0];
        console.log(seen, window.event === undefined, typeof nav.responseStart, performance.timing.navigationStart > 0,
                    window instanceof Window, window instanceof EventTarget, escape('a bé中'), unescape('%u4E2D%41'));
        </script>)");
    EXPECT_EQ(p.console, "log:hi1true true number true true true a%20b%E9%u4E2D 中A\n") << p.console;
}

TEST(Script, AttributesByNameAndALiveDataset) {
    Page p(R"(<body><div id=d data-user-id=7 title=t></div><script>
        const d = document.getElementById('d');
        const before = [d.attributes.title.value, d.attributes.getNamedItem('id').nodeType, d.attributes.item(9), d.dataset.userId];
        d.dataset.fooBar = 'x';
        delete d.dataset.userId;
        console.log(before.join(), d.getAttribute('data-foo-bar'), d.hasAttribute('data-user-id'), Object.keys(d.dataset).join(),
                    'onsubmit' in window, window.onsubmit);
        </script>)");
    EXPECT_EQ(p.console, "log:t,2,,7 x false fooBar true null\n") << p.console;
}

TEST(Script, MessageChannelDeliversAsATask) {
    Page p(R"(<body><script>
        const c = new MessageChannel();
        const got = [];
        c.port2.onmessage = (e) => { got.push(e.data.n); if (got.length === 2) console.log(got.join(), e instanceof MessageEvent); };
        c.port1.postMessage({ n: 1 });
        c.port1.postMessage({ n: 2 });
        console.log('sync', got.length);
        </script>)");
    for (int i = 0; i < 40 && p.script->busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p.script->tick();
    }
    EXPECT_EQ(p.console, "log:sync 0\nlog:1,2 true\n") << p.console;
}

TEST(Script, NodeConstantsAndCustomElementsMadeInsideAnUpgrade) {
    // ShadyDOM reads Node.DOCUMENT_FRAGMENT_NODE; Polymer's constructors make other elements.
    Page p(R"(<body><x-outer></x-outer><script>
        class Inner extends HTMLElement {}
        customElements.define('x-inner', Inner);
        class Outer extends HTMLElement { constructor() { super(); this.made = new Inner(); } }
        customElements.define('x-outer', Outer);
        const o = document.querySelector('x-outer');
        console.log(Node.DOCUMENT_FRAGMENT_NODE, Node.DOCUMENT_POSITION_CONTAINED_BY, document.body.ATTRIBUTE_NODE,
                    o instanceof Outer, o.made instanceof Inner);
        </script>)");
    EXPECT_EQ(p.console, "log:11 16 2 true true\n") << p.console;
}

TEST(Script, PostMessageToItselfArrivesAsATask) {
    Page p(R"(<body><script>
        const got = [];
        window.addEventListener('message', (e) => { got.push(e.data.k + ':' + (e.source === window) + ':' + e.origin); });
        window.onmessage = (e) => { if (got.length === 1) console.log(got.join(), 'onmessage too'); };
        window.postMessage({ k: 'v' }, '*');
        window.postMessage({ k: 'nope' }, 'https://elsewhere.example');
        console.log('sync', got.length);
        </script>)");
    for (int i = 0; i < 40 && p.script->busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p.script->tick();
    }
    EXPECT_EQ(p.console, "log:sync 0\nlog:v:true:https://example.org onmessage too\n") << p.console;
}

TEST(Script, InnerTextIsTheTextAsShown) {
    Page p(R"(<body><div id=d>  Hello
        <b>big</b>   world<script>var x = 1;</script><style>p{}</style><p>para</p><span hidden>no</span>a<br>b<ul><li>one</li><li>two</li></ul></div>
        <script>console.log(JSON.stringify(document.getElementById('d').innerText));</script>)");
    EXPECT_EQ(p.console, "log:\"Hello big world\\n\\npara\\n\\na\\nb\\none\\ntwo\"\n") << p.console;
}

TEST(Script, DocumentFontsAnswersAtOnce) {
    Page p(R"(<body><script>
        document.fonts.load("italic 700 16px 'Source Sans', serif", 'BESbswy').then(fs => {
          const f = new FontFace('X', 'url(x.woff2)', { weight: '700' });
          document.fonts.add(f);
          f.load().then(g => console.log(fs.length, fs[0].family, document.fonts.check('16px X'), document.fonts.size, g === f, g.status));
        });
        </script>)");
    for (int i = 0; i < 40 && p.script->busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p.script->tick();
    }
    EXPECT_EQ(p.console, "log:1 Source Sans true 1 true loaded\n") << p.console;
}

TEST(Script, MoreReflectedAttributes) {
    Page p(R"(<head><link rel=alternate hreflang=en-GB href=/uk><link id=l rel=stylesheet href=x.css crossorigin></head><body><script>
        const alt = document.querySelector('[hreflang]');
        const l = document.getElementById('l');
        l.referrerPolicy = 'no-referrer';
        console.log(alt.hreflang.split('-')[1], l.crossOrigin, l.getAttribute('referrerpolicy'), document.body.crossOrigin, alt.media === '');
        </script>)");
    EXPECT_EQ(p.console, "log:GB anonymous no-referrer undefined true\n") << p.console;   // <body> has no crossOrigin
}

TEST(Script, Canvas2dDrawsPixels) {
    Page p(R"(<body><canvas id=c width=20 height=10></canvas><script>
        const c = document.getElementById('c'), x = c.getContext('2d');
        x.fillStyle = '#ff0000';
        x.fillRect(0, 0, 10, 10);
        x.save(); x.translate(10, 0); x.fillStyle = 'rgb(0, 0, 255)'; x.beginPath(); x.rect(0, 0, 10, 10); x.fill(); x.restore();
        x.clearRect(0, 0, 2, 2);
        const px = (X, Y) => Array.from(x.getImageData(X, Y, 1, 1).data).join(',');
        const g = x.createLinearGradient(0, 0, 20, 0); g.addColorStop(0, 'black'); g.addColorStop(1, 'white');
        const img = x.createImageData(1, 1); img.data.set([1, 2, 3, 255]); x.putImageData(img, 19, 9);
        const p2 = new Path2D('M0 0 L4 0 L4 4 Z');
        console.log(px(5, 5), px(15, 5), px(0, 0), px(19, 9), c.toDataURL().slice(0, 22), x.isPointInPath(p2, 3, 1), x.isPointInPath(p2, 1, 3),
                    c.getContext('2d') === x, c.getContext('webgl'), x.measureText('ab').width > 0, c.width, c.height);
        </script>)");
    EXPECT_EQ(p.console, "log:255,0,0,255 0,0,255,255 0,0,0,0 1,2,3,255 data:image/png;base64, true false true null true 20 10\n") << p.console;
}

TEST(Script, CanvasTextIsPlacedByAlignAndBaseline) {
    Page p(R"(<body><canvas id=c width=40 height=30></canvas><script>
        const x = document.getElementById('c').getContext('2d');
        x.fillStyle = '#00ff00';
        x.textAlign = 'right'; x.textBaseline = 'top';
        x.fillText('ab', 30, 5);   // 10 wide, 10 tall: x 20..30, y 5..15
        const a = (X, Y) => x.getImageData(X, Y, 1, 1).data[3];
        console.log(x.measureText('abcd').width, a(25, 10), a(15, 10), a(25, 3), a(25, 17));
        </script>)");
    EXPECT_EQ(p.console, "log:20 255 0 0 0\n") << p.console;
}

TEST(Script, IntlFormattersCanBeCalledWithoutNew) {
    Page p(R"(<body><script>
        const d = Intl.DateTimeFormat(), n = Intl.NumberFormat('en-US');
        let threw = false;
        try { Intl.PluralRules(); } catch (e) { threw = e instanceof TypeError; }
        console.log(typeof d.resolvedOptions().timeZone, n.format(1234.5), d instanceof Intl.DateTimeFormat, typeof Intl.Collator().compare, threw);
        </script>)");
    EXPECT_EQ(p.console, "log:string 1,234.5 true function true\n") << p.console;
}

TEST(Script, OffsetParentEndsAtTheBody) {
    // Bing walks `do { ... } while (n = n.offsetParent)`: it has to end.
    Page p(R"(<body><div id=d><span id=s>x</span></div><script>
        let n = document.getElementById('s'), hops = 0;
        do { hops++; } while ((n = n.offsetParent) && hops < 100);
        console.log(hops, document.body.offsetParent, document.documentElement.offsetParent, document.createElement('i').offsetParent,
                    document.getElementById('s').offsetParent === document.body);
        </script>)");
    EXPECT_EQ(p.console, "log:2 null null null true\n") << p.console;
}

TEST(Script, ScriptsLinksAndTitlesHaveText) {
    Page p(R"(<head><title>T</title><script type="text/rms">var fromRms = 7;</script></head><body><a href=#>link</a><script>
        const rms = [...document.scripts].find(s => s.type === 'text/rms');
        (0, eval)(rms.text);
        console.log(fromRms, document.querySelector('a').text, document.querySelector('title').text);
        </script>)");
    EXPECT_EQ(p.console, "log:7 link T\n") << p.console;
}

TEST(Script, ChildNodesAndChildrenAreLive) {
    Page p(R"(<body><div id=d><i></i><b class=x></b>text</div><script>
        const d = document.getElementById('d');
        const nodes = d.childNodes, kids = d.children, xs = document.getElementsByClassName('x'), bs = d.getElementsByTagName('b');
        const before = [nodes.length, kids.length, xs.length, bs.length];
        const f = document.createDocumentFragment();
        let moved = 0;
        for (; nodes.length && moved < 100; moved++) f.appendChild(nodes[0]);   // Bing's sj_appHTML
        const b = document.createElement('b'); b.className = 'x'; d.appendChild(b);
        console.log(before.join(), moved, nodes.length, kids.length, xs.length, bs.length, d.childNodes === nodes,
                    nodes instanceof NodeList, Array.isArray(nodes), [...nodes].length, Array.from(kids)[0] === b, nodes.item(0) === b);
        </script>)");
    EXPECT_EQ(p.console, "log:3,2,1,1 3 1 1 1 1 true true true 1 true true\n") << p.console;
}

TEST(Script, LiveListsSurviveCollection) {
    Page p(R"(<head><title>t</title></head><body><script>
        const l = document.getElementsByTagName('head');
        const first = l[0];
        let junk = [];
        for (let i = 0; i < 200000; i++) { junk.push({ i, s: 'x' + i }); if (junk.length > 1000) junk = []; }
        const again = document.getElementsByTagName('head');
        console.log(l.length, l[0] === first, again[0] === first, Array.from(again).length, !!again[0]);
        </script>)");
    EXPECT_EQ(p.console, "log:1 true true 1 true\n") << p.console;
}

TEST(Script, PerformanceEntriesHaveTheirInterfaces) {
    Page p(R"(<body><script>
        performance.mark('m');
        const [nav] = performance.getEntriesByType('navigation');
        console.log(nav instanceof PerformanceNavigationTiming, nav instanceof PerformanceEntry,
                    performance.getEntriesByName('m')[0] instanceof PerformanceMark);
        </script>)");
    EXPECT_EQ(p.console, "log:true true true\n") << p.console;
}

TEST(Script, ACustomElementsOwnPropertiesStayItsOwn) {
    // Reddit's <zoomable-img> keeps its <img> in this.media: not the <link media> attribute.
    Page p(R"(<body><zoomable-img><img src="a.png" alt=""></zoomable-img><link id=l media=print><script>
        class Z extends HTMLElement {
          connectedCallback() { this.media = this.querySelector('img'); this.sizes = 3; }
        }
        customElements.define('zoomable-img', Z);
        const z = document.querySelector('zoomable-img');
        console.log(typeof z.media.addEventListener, z.sizes, z.hasAttribute('media'), document.getElementById('l').media);
        </script>)");
    EXPECT_EQ(p.console, "log:function 3 false print\n") << p.console;
}

TEST(Script, ElementsSayTheirNamespace) {
    // Vue's hydration asks namespaceURI.includes("svg") of every element (namu.wiki).
    Page p(R"(<body><svg><circle/></svg><script>
        console.log(document.body.namespaceURI, document.querySelector('circle').namespaceURI.includes('svg'),
                    document.createElement('div').prefix);
        </script>)");
    EXPECT_EQ(p.console, "log:http://www.w3.org/1999/xhtml true null\n") << p.console;
}

TEST(Script, QuerySelectorKnowsHas) {
    // CNN's menu script asks for ':has(...)' with an attribute selector inside.
    Page p(R"(<body><ul><li class=s><a class=l data-t="Live TV">x</a></li><li class=s><a class=l>y</a></li></ul><script>
        console.log(document.querySelectorAll('.s:has(.l[data-t="Live TV"])').length, document.querySelectorAll('li:has(+ li)').length,
                    document.querySelector('ul:has(> li > a)') !== null, document.querySelectorAll('li:not(:has(a[data-t]))').length);
        </script>)");
    EXPECT_EQ(p.console, "log:1 1 true 1\n") << p.console;
}

TEST(Script, AScriptsPromisesStillSeeItAsCurrent) {
    // The microtask checkpoint comes before currentScript is restored (Turbopack's
    // runtime awaits, then asks for document.currentScript.src).
    Page p(R"(<body><script id=s>
        Promise.resolve().then(() => console.log(document.currentScript && document.currentScript.id));
        setTimeout(() => console.log(document.currentScript));
        </script>)");
    p.script->tick();
    EXPECT_EQ(p.console, "log:s\nlog:null\n") << p.console;
}

TEST(Script, ReadableStreamsRead) {
    // X's server-rendered data comes as new ReadableStream({ start(controller) { ... } }).
    Page p(R"(<body><script>
        const out = [];
        const s = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } });
        (async () => {
          for await (const v of s) out.push(v);
          let n = 0;
          const pulled = new ReadableStream({ pull(c) { if (++n > 3) c.close(); else c.enqueue(n * 10); } });
          const [a, b] = pulled.tee();
          const ra = a.getReader();
          let r;
          while (!(r = await ra.read()).done) out.push(r.value);
          const t = new TransformStream({ transform(ch, c) { c.enqueue(ch + 1); } });
          const piped = b.pipeThrough(t).getReader();
          while (!(r = await piped.read()).done) out.push(r.value);
          const e = new ReadableStream({ start(c) { c.error(new Error('x')); } });
          try { await e.getReader().read(); } catch (err) { out.push(err.message); }
          console.log(out.join(), s.locked, typeof performance.setResourceTimingBufferSize);
        })();
        </script>)");
    for (int i = 0; i < 5; ++i) p.script->tick();
    EXPECT_EQ(p.console, "log:1,2,10,20,30,11,21,31,x false function\n") << p.console;
}

TEST(Script, OnHandlersRunOnce) {
    // Angular's $httpBackend nulls its request in onload: a second call threw.
    Page p(R"(<body><script>
        let aborts = 0, messages = 0;
        const c = new AbortController();
        c.signal.onabort = () => aborts++;
        c.abort();
        const a = new BroadcastChannel('t'), b = new BroadcastChannel('t');
        b.onmessage = () => messages++;
        a.postMessage(1);
        setTimeout(() => setTimeout(() => console.log(aborts, messages)));
        </script>)");
    for (int i = 0; i < 5; ++i) p.script->tick();
    EXPECT_EQ(p.console, "log:1 1\n") << p.console;
}

TEST(Script, ResponseBodiesAreStreamsAndSelectionIsThere) {
    // Tistory (Next.js) reads responses with body.getReader() and asks getSelection().
    Page p(R"(<body><script>
        (async () => {
          const r = new Response('hey');
          const reader = r.body.getReader();
          const first = await reader.read(), end = await reader.read();
          const sel = getSelection();
          sel.removeAllRanges();
          console.log(new TextDecoder().decode(first.value), end.done, r.bodyUsed, new Response(null).body,
                      sel.rangeCount, String(sel), document.getSelection() === sel);
        })();
        </script>)");
    for (int i = 0; i < 5; ++i) p.script->tick();
    EXPECT_EQ(p.console, "log:hey true true null 0  true\n") << p.console;
}

TEST(Script, DatasetIsADOMStringMap) {
    Page p(R"(<body data-x-y=1><script>
        console.log(document.body.dataset instanceof DOMStringMap, document.body.dataset.xY);
        </script>)");
    EXPECT_EQ(p.console, "log:true 1\n") << p.console;
}

TEST(Script, TextDecoderKnowsEucKr) {
    Page p(R"(<body><script>
        const d = new TextDecoder('ks_c_5601-1987');
        console.log(d.encoding, d.decode(new Uint8Array([0xC7, 0xD1, 0xB1, 0xDB])));
        </script>)");
    EXPECT_EQ(p.console, "log:euc-kr \xED\x95\x9C\xEA\xB8\x80\n") << p.console;
}

TEST(Script, CompareDocumentPosition) {
    Page p(R"(<body><div id=a><span id=b></span></div><p id=c></p><script>
        const a = document.getElementById('a'), b = document.getElementById('b'), c = document.getElementById('c');
        console.log(a.compareDocumentPosition(b), b.compareDocumentPosition(a), a.compareDocumentPosition(c), c.compareDocumentPosition(b),
                    a.compareDocumentPosition(a), a.compareDocumentPosition(document.createElement('i')) & 1);
        </script>)");
    EXPECT_EQ(p.console, "log:20 10 4 2 0 1\n") << p.console;
}

TEST(Script, SplitTextMediaErrorJavaAndGenericSplice) {
    Page p(R"(<body><p id=p>hello world</p><script>
        const t = document.getElementById('p').firstChild;
        const rest = t.splitText(5);
        const like = { 0: 'a', 1: 'b', 2: 'c', length: 3 };
        const gone = Array.prototype.splice.call(like, 1, 1, 'x', 'y');
        console.log(t.data + '|' + rest.data, document.getElementById('p').childNodes.length, rest.wholeText,
                    MediaError.MEDIA_ERR_NETWORK, navigator.javaEnabled(), gone.join(), like.length, like[1] + like[2] + like[3]);
        </script>)");
    EXPECT_EQ(p.console, "log:hello| world 2 hello world 2 false b 4 xyc\n") << p.console;
}

TEST(Script, SvgInterfacesAndCheckVisibility) {
    Page p(R"(<body><svg><a><path/></a><linearGradient/></svg><div id=h style="display:none"><b id=b></b></div><i id=i></i><script>
        const a = document.querySelector('svg a'), path = document.querySelector('path'), lg = document.querySelector('linearGradient');
        console.log(a instanceof SVGAElement, path instanceof SVGGeometryElement, path instanceof SVGGraphicsElement,
                    lg instanceof SVGGradientElement, document.querySelector('svg') instanceof SVGSVGElement,
                    document.getElementById('b').checkVisibility(), document.getElementById('i').checkVisibility());
        </script>)");
    EXPECT_EQ(p.console, "log:true true true true true false true\n") << p.console;
}

TEST(Script, AStylePreloadHearsItsSheetCome) {
    // loadCSS (samsung.com/sec): <link rel=preload as=style onload="this.rel='stylesheet'">.
    Page p(R"(<head><link id=l rel=preload as=style href="https://example.org/a.css" onload="this.onload=null;this.rel='stylesheet'">
        <link id=m rel=preload as=script href="https://example.org/a.css"></head><body><script>
        document.getElementById('m').onload = () => console.log('wrong one');
        </script>)");
    p.script->sheetLoaded("https://example.org/a.css", true);
    std::string rel;
    std::vector<const Node*> todo{p.doc->root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (const std::string* id = n->attr("id"); id && *id == "l") rel = *n->attr("rel");
        for (const Node* k : n->children) todo.push_back(k);
    }
    EXPECT_EQ(rel, "stylesheet");
    EXPECT_EQ(p.console, "") << p.console;
}

TEST(Script, DomMatrix) {
    Page p(R"(<body><script>
        const m = new DOMMatrix('translate(10px, 20px) scale(2)');
        const p = m.transformPoint(new DOMPoint(1, 1));
        const inv = m.inverse().transformPoint(p);
        const r = new DOMMatrix().rotate(90);
        console.log(String(m), p.x, p.y, Math.round(inv.x), Math.round(inv.y), m.is2D, new DOMMatrix().isIdentity,
                    Math.round(r.b), new DOMMatrix([1, 0, 0, 1, 5, 6]).multiply(new DOMMatrix([2, 0, 0, 2, 0, 0])).e,
                    DOMMatrix.fromMatrix({ a: 3, d: 3 }).a);
        </script>)");
    EXPECT_EQ(p.console, "log:matrix(2, 0, 0, 2, 10, 20) 12 22 1 1 true true 1 5 3\n") << p.console;
}

TEST(Script, TokenListsOverOtherAttributes) {
    Page p(R"(<body><iframe id=f sandbox="allow-scripts allow-same-origin"></iframe><a id=a rel="noopener"></a><script>
        const f = document.getElementById('f'), a = document.getElementById('a');
        f.sandbox.add('allow-popups');
        a.relList.toggle('noreferrer');
        console.log(f.sandbox.toString(), f.sandbox.length, f.sandbox[1], f.sandbox.contains('allow-scripts'), a.rel, f.allow === '');
        </script>)");
    EXPECT_EQ(p.console, "log:allow-scripts allow-same-origin allow-popups 3 allow-same-origin true noopener noreferrer true\n") << p.console;
}

TEST(Script, DatesInTheirTimeZones) {
    // The Verge renders New York times on the server; hydrating with another zone's text
    // made React throw the page away (#418).
    Page p(R"(<body><script>
        const d = new Date(Date.UTC(2026, 10, 12, 18, 5, 0)), summer = new Date(Date.UTC(2026, 6, 1, 12, 0, 0));
        const f = (o, x = d) => new Intl.DateTimeFormat('en-US', o).format(x);
        console.log([f({ month: 'long', day: 'numeric', year: 'numeric', timeZone: 'America/New_York' }),
                     f({ hour: 'numeric', minute: '2-digit', timeZone: 'America/New_York', timeZoneName: 'short' }),
                     f({ hour: 'numeric', minute: '2-digit', timeZone: 'America/New_York', timeZoneName: 'short' }, summer),
                     f({ hour: 'numeric', minute: '2-digit', timeZone: 'Europe/Berlin' }, summer),
                     f({ hour: 'numeric', timeZone: 'Australia/Sydney' }), f({ day: 'numeric', timeZone: 'Asia/Seoul' }),
                     d.toLocaleTimeString('en-US', { timeZone: 'UTC' }), d.toLocaleString('en-US', { timeZone: 'UTC' })].join(' | '));
        </script>)");
    EXPECT_EQ(p.console, "log:November 12, 2026 | 1:05 PM EST | 8:00 AM EDT | 2:00 PM | 5 AM | 13 | 6:05:00 PM | 11/12/2026, 6:05:00 PM\n") << p.console;
}

TEST(Script, FrameDocumentsAttrNodesAndSmallGlobals) {
    Page p(R"(<body><iframe id=f></iframe><a id=a href="/x" title="t"></a><script>
        const f = document.getElementById('f'), d = f.contentWindow.document.open();
        d.write('<p>hi</p>');
        d.close();
        const a = document.getElementById('a'), n = a.getAttributeNode('title');
        n.value = 'u';
        console.log(!!f.contentDocument.body, f.contentDocument === d, a.getAttribute('title'), a.getAttributeNode('nope'),
                    typeof IntersectionObserverEntry, Notification.permission);
        </script>)");
    EXPECT_EQ(p.console, "log:true true u null function denied\n") << p.console;
}

TEST(Script, DocumentWriteIsOneStreamPerScript) {
    // yes24's ad server writes a <script> tag in pieces.
    Page p(R"(<body><div id=d><script>
        var seen = [];
        document.write('<script type=');
        document.write("'");
        document.write("text/javascript'>seen.push('ran');");
        document.write('</scr' + 'ipt><b>bold</b>');
        </script></div><script>console.log(seen.join(), document.querySelector('#d b').textContent, document.querySelectorAll('#d script').length);</script>)");
    EXPECT_EQ(p.console, "log:ran bold 2\n") << p.console;
}

TEST(Script, NavigatorPluginsAreThereAndEmpty) {
    Page p(R"(<body><script>
        console.log(navigator.plugins.length, navigator.mimeTypes.length, navigator.plugins.namedItem('x'), [...navigator.plugins].length, navigator.pdfViewerEnabled);
        </script>)");
    EXPECT_EQ(p.console, "log:0 0 null 0 false\n") << p.console;
}

TEST(Script, SvgAttributesKeepTheirCase) {
    Page p(R"(<body><svg id=s></svg><div id=d></div><script>
        const s = document.getElementById('s'), d = document.getElementById('d');
        s.setAttribute('viewBox', '0 0 24 24');
        d.setAttribute('DATA-X', '1');
        console.log(s.getAttributeNames().join(), s.getAttribute('viewBox'), s.hasAttribute('viewbox'), d.getAttributeNames().join(), d.getAttribute('data-X'));
        </script>)");
    EXPECT_EQ(p.console, "log:id,viewBox 0 0 24 24 false id,data-x 1\n") << p.console;
}

TEST(Script, DocumentAllIsHTMLDDA) {
    Page p(R"(<body><p id=x name=n></p><script>
        const u = undefined;
        console.log(typeof document.all, !document.all, document.all == null, document.all === undefined,
                    !u && u !== document.all, document.all.length > 3, document.all.x === document.getElementById('x'),
                    document.all('x') === document.all.namedItem('x'), document.all[0].tagName);
        </script>)");
    EXPECT_EQ(p.console, "log:undefined true true false true true true true HTML\n") << p.console;
}

TEST(Script, NamespacedAttributes) {
    Page p(R"(<body><svg><use id=u></use></svg><script>
        const u = document.getElementById('u');
        u.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', '#a');
        console.log(u.getAttribute('xlink:href'), u.getAttributeNS('http://www.w3.org/1999/xlink', 'href'), u.hasAttributeNS(null, 'href'));
        u.removeAttributeNS('http://www.w3.org/1999/xlink', 'href');
        console.log(u.hasAttribute('xlink:href'));
        </script>)");
    EXPECT_EQ(p.console, "log:#a #a true\nlog:false\n") << p.console;
}

TEST(Script, IntlLocaleAndComputedDefaults) {
    Page p(R"(<body><p id=p></p><script>
        const l = new Intl.Locale('ko-kr-u-ca-gregory', { hourCycle: 'h23' });
        const cs = getComputedStyle(document.getElementById('p'));
        console.log(l.language, l.region, l.baseName, l.calendar, l.hourCycle, String(l), new Intl.Locale('en').maximize().baseName,
                    cs.content, typeof cs.cursor, cs.getPropertyValue('pointer-events'));
        </script>)");
    EXPECT_EQ(p.console, "log:ko KR ko-KR gregory h23 ko-KR-u-ca-gregory-hc-h23 en-Latn-US normal string auto\n") << p.console;
}

TEST(Script, MetaContentReflects) {
    Page p(R"(<head><meta name=x content="hello"></head><body><script>
        const m = document.querySelector('meta[name=x]');
        console.log(m.content, document.body.content);
        </script>)");
    EXPECT_EQ(p.console, "log:hello undefined\n") << p.console;
}

TEST(Script, StorageIsAnInterface) {
    Page p(R"(<body><script>
        localStorage.setItem('a', '1');
        console.log(localStorage instanceof Storage, sessionStorage instanceof Storage, localStorage.getItem('a'));
        </script>)");
    EXPECT_EQ(p.console, "log:true true 1\n") << p.console;
}
