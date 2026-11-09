// The Fetch Standard's part the browser keeps for itself, out of the page's reach: which
// headers a page may send, whether cookies go, the referrer, and CORS (preflights, which
// responses a page may read, and which of their headers). The page's fetch() and
// XMLHttpRequest (script_prelude.cc) come here through __xwebFetch; the viewer below
// only carries requests (and keeps pages on the web away from this machine).

#include "script_impl.h"

namespace fbx::xweb {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// scheme://host[:port], or "null" for what has no origin.
std::string originOf(const std::string& url) {
    const Url u = parseUrl(url);
    if ((u.scheme != "http" && u.scheme != "https") || !u.has_authority) return "null";
    std::string host = lowerAscii(u.authority);
    if (const size_t at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
    if (u.scheme == "http" && host.ends_with(":80")) host.resize(host.size() - 3);
    if (u.scheme == "https" && host.ends_with(":443")) host.resize(host.size() - 4);
    return u.scheme + "://" + host;
}

bool forbiddenRequestHeader(const std::string& name) {
    static const char* const kForbidden[] = {"accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
                                             "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
                                             "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via"};
    for (const char* f : kForbidden)
        if (name == f) return true;
    return name.starts_with("proxy-") || name.starts_with("sec-");
}

// A request header CORS lets through without asking first.
bool safelistedRequestHeader(const std::string& name, const std::string& value) {
    if (value.size() > 128) return false;
    if (name == "accept" || name == "accept-language" || name == "content-language") return true;
    if (name == "content-type") {
        std::string type = lowerAscii(value.substr(0, value.find(';')));
        while (!type.empty() && type.back() == ' ') type.pop_back();
        return type == "application/x-www-form-urlencoded" || type == "multipart/form-data" || type == "text/plain";
    }
    return false;
}

bool safelistedResponseHeader(const std::string& name) {
    for (const char* h : {"cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma"})
        if (name == h) return true;
    return false;
}

std::string headerValue(const std::vector<std::pair<std::string, std::string>>& headers, const std::string& name) {
    std::string out;
    for (const auto& [k, v] : headers)
        if (k == name) out += (out.empty() ? "" : ", ") + v;
    return out;
}

std::vector<std::string> tokens(const std::string& list) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= list.size()) {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        std::string t = list.substr(at, end - at);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (!t.empty()) out.push_back(lowerAscii(t));
        at = end + 1;
    }
    return out;
}

// What a CORS check says about a response: allowed, or why not.
std::string corsCheck(const PageScript::Host::Response& r, const std::string& origin, bool credentials) {
    const std::string acao = headerValue(r.headers, "access-control-allow-origin");
    if (acao.empty()) return "the server doesn't allow this page to read its response (no Access-Control-Allow-Origin)";
    if (acao == "*") {
        if (credentials) return "a credentialed request can't be answered with Access-Control-Allow-Origin: *";
    } else if (acao != origin) {
        return "Access-Control-Allow-Origin is " + acao + ", not " + origin;
    }
    if (credentials && headerValue(r.headers, "access-control-allow-credentials") != "true")
        return "the server doesn't allow credentials (Access-Control-Allow-Credentials)";
    return {};
}

}  // namespace

// __xwebFetch(url, method, headers [[name, value]...], body, mode, credentials) → a promise
// of [status, url, contentType, ArrayBuffer, [[name, value]...], type], rejected with a
// TypeError when there's no response the page may have.
Value PageScript::Impl::browserFetch(Args a) {
    Object* promise = rt.newPromise();
    const auto refuse = [&](const std::string& why) {
        rt.rejectPromise(promise, Value::object(rt.newError("TypeError", "Failed to fetch: " + why)));
        return Value::object(promise);
    };
    auto req = std::make_shared<Host::Request>();
    req->url = resolveUrl(host.base, js::utf8(rt.toStringValue(arg(a, 0))));
    if (const size_t hash = req->url.find('#'); hash != std::string::npos) req->url.resize(hash);
    req->method = arg(a, 1).isUndefined() ? "GET" : upper(js::utf8(rt.toStringValue(arg(a, 1))));
    const std::string mode = arg(a, 4).isUndefined() ? "cors" : js::utf8(rt.toStringValue(arg(a, 4)));
    const std::string credentials = arg(a, 5).isUndefined() ? "same-origin" : js::utf8(rt.toStringValue(arg(a, 5)));
    // The page's headers, without those only the browser sets.
    std::vector<std::pair<std::string, std::string>> page_headers;
    if (arg(a, 2).isObject()) {
        for (const Value& pair : rt.iterateToList(arg(a, 2))) {
            if (!pair.isObject()) continue;
            const std::string name = lowerAscii(js::utf8(rt.toStringValue(rt.get(pair, u"0"))));
            const std::string value = js::utf8(rt.toStringValue(rt.get(pair, u"1")));
            if (name.empty() || name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos) continue;
            if (forbiddenRequestHeader(name)) continue;
            page_headers.emplace_back(name, value);
        }
    }
    if (arg(a, 3).isString()) req->body = js::utf8(arg(a, 3).asString());
    else if (!arg(a, 3).isNullish() && !rt.bufferBytes(arg(a, 3), req->body)) req->body = js::utf8(rt.toStringValue(arg(a, 3)));
    req->content_type = headerValue(page_headers, "content-type");
    if (!host.fetch) return refuse("no network here");
    const Url target = parseUrl(req->url);
    if (target.scheme != "http" && target.scheme != "https") return refuse("only http and https can be fetched");

    const std::string origin = originOf(host.url);
    const bool same = originOf(req->url) == origin;
    if (mode == "same-origin" && !same) return refuse("a same-origin request to another origin");
    const bool opaque = mode == "no-cors" && !same;
    if (opaque) {
        if (req->method != "GET" && req->method != "HEAD" && req->method != "POST") return refuse("no-cors allows only GET, HEAD and POST");
        std::erase_if(page_headers, [](const auto& h) { return !safelistedRequestHeader(h.first, h.second); });
    }
    req->cookies = credentials == "include" || (credentials == "same-origin" && same);
    req->headers = page_headers;
    // Origin for what crosses origins or changes things; the referrer as
    // strict-origin-when-cross-origin has it.
    if (!same || (req->method != "GET" && req->method != "HEAD")) req->headers.emplace_back("Origin", origin);
    {
        const Url page = parseUrl(host.url);
        const bool downgrade = page.scheme == "https" && target.scheme == "http";
        if (!downgrade && (page.scheme == "http" || page.scheme == "https"))
            req->headers.emplace_back("Referer", same ? parseUrl(host.url).withoutFragment() : origin + "/");
    }
    const bool simple = (req->method == "GET" || req->method == "HEAD" || req->method == "POST") &&
                        std::all_of(page_headers.begin(), page_headers.end(), [](const auto& h) { return safelistedRequestHeader(h.first, h.second); });
    const bool with_credentials = credentials == "include";

    pending_fetches.push_back(promise);
    std::weak_ptr<bool> alive = this->alive;
    Impl* self = this;
    // Settle the page's promise from a response the page may have (or not).
    auto finish = [self, alive, promise, origin, same, opaque, with_credentials](Host::Response r) {
        if (alive.expired()) return;
        auto& pend = self->pending_fetches;
        pend.erase(std::remove(pend.begin(), pend.end(), promise), pend.end());
        self->rt.setStepBudget(kTaskBudget);
        self->task([&] {
            Runtime& rt = self->rt;
            const auto fail = [&](const std::string& why) {
                rt.rejectPromise(promise, Value::object(rt.newError("TypeError", "Failed to fetch" + (why.empty() ? std::string() : ": " + why))));
            };
            if (!r.ok) return fail(r.error);
            // A same-origin request that ended up elsewhere is a cross-origin response.
            const bool cross = !same || originOf(r.url) != origin;
            std::string type = cross ? "cors" : "basic";
            std::vector<std::pair<std::string, std::string>> shown;
            if (opaque) {
                type = "opaque";
                r.status = 0;
                r.body.clear();
                r.content_type.clear();
                r.url.clear();
            } else if (cross) {
                if (const std::string why = corsCheck(r, origin, with_credentials); !why.empty()) {
                    self->report("CORS: " + why + " (" + r.url + ")");
                    return fail("CORS");
                }
                const std::vector<std::string> exposed = tokens(headerValue(r.headers, "access-control-expose-headers"));
                const bool all = !with_credentials && std::find(exposed.begin(), exposed.end(), "*") != exposed.end();
                for (const auto& h : r.headers)
                    if (all || safelistedResponseHeader(h.first) || std::find(exposed.begin(), exposed.end(), h.first) != exposed.end()) shown.push_back(h);
            } else {
                shown = r.headers;
            }
            std::vector<Value> header_values;
            for (const auto& [k, v] : shown) header_values.push_back(Value::object(rt.newArray({str(k), str(v)})));
            Object* parts = rt.newArray({Value::number(r.status), str(r.url), str(r.content_type), Value::object(rt.newArrayBuffer(r.body)),
                                         Value::object(rt.newArray(std::move(header_values))), str(type)});
            rt.resolvePromise(promise, Value::object(parts));
        });
        self->drainMicrotasks();
        self->dirty = true;
    };
    if (same || opaque || simple) {
        host.fetch(*req, finish);
        return Value::object(promise);
    }
    // A preflight first: may this page send this method and these headers?
    Host::Request pre;
    pre.url = req->url;
    pre.method = "OPTIONS";
    pre.headers.emplace_back("Origin", origin);
    pre.headers.emplace_back("Access-Control-Request-Method", req->method);
    std::vector<std::string> asked;
    for (const auto& h : page_headers)
        if (!safelistedRequestHeader(h.first, h.second)) asked.push_back(h.first);
    std::sort(asked.begin(), asked.end());
    asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
    if (!asked.empty()) {
        std::string list;
        for (const auto& n : asked) list += (list.empty() ? "" : ",") + n;
        pre.headers.emplace_back("Access-Control-Request-Headers", list);
    }
    host.fetch(pre, [self, alive, req, finish, origin, with_credentials, asked](Host::Response r) {
        if (alive.expired()) return;
        std::string why;
        if (!r.ok) why = r.error.empty() ? "the preflight request failed" : r.error;
        else if (r.status < 200 || r.status >= 300) why = "the preflight was answered " + std::to_string(r.status);
        else why = corsCheck(r, origin, with_credentials);
        if (why.empty()) {
            const std::vector<std::string> methods = tokens(headerValue(r.headers, "access-control-allow-methods"));
            const bool any_method = !with_credentials && std::find(methods.begin(), methods.end(), "*") != methods.end();
            if (!any_method && std::find(methods.begin(), methods.end(), lowerAscii(req->method)) == methods.end() &&
                req->method != "GET" && req->method != "HEAD" && req->method != "POST")
                why = "the method " + req->method + " isn't allowed (Access-Control-Allow-Methods)";
        }
        if (why.empty()) {
            const std::vector<std::string> allowed = tokens(headerValue(r.headers, "access-control-allow-headers"));
            const bool any_header = !with_credentials && std::find(allowed.begin(), allowed.end(), "*") != allowed.end();
            for (const auto& n : asked)
                if (!any_header && std::find(allowed.begin(), allowed.end(), n) == allowed.end()) why = "the header " + n + " isn't allowed (Access-Control-Allow-Headers)";
        }
        if (!why.empty()) {
            Host::Response refused;
            refused.error = "CORS preflight: " + why;
            self->report("CORS: " + why + " (" + req->url + ")");
            finish(refused);
            return;
        }
        self->host.fetch(*req, finish);
    });
    return Value::object(promise);
}

}  // namespace fbx::xweb
