// What a page's script world gets that's simplest written in JavaScript itself: URL
// and URLSearchParams (the WHATWG URL Standard's API, over an RFC 3986 parse with the
// special schemes' rules: lower-case scheme and host, default ports dropped, "/" for an
// empty path, dot segments resolved); AbortController and AbortSignal; NodeFilter,
// TreeWalker and NodeIterator over the tree's own links.

namespace fbx::xweb {

extern const char* const kScriptPrelude;

const char* const kScriptPrelude = R"JS(
(function (global) {
  'use strict';
  const special = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21', 'file:': '' };
  const state = Symbol('url');
  const params = Symbol('params');
  const owner = Symbol('owner');
  const re = /^(?:([A-Za-z][A-Za-z0-9+.\-]*):)?(?:\/\/([^\/?#]*))?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/;

  function removeDots(path) {
    const out = [];
    const segs = path.split('/');
    for (let i = 0; i < segs.length; i++) {
      const s = segs[i];
      if (s === '..' || s.toLowerCase() === '%2e%2e') { if (out.length > 1) out.pop(); if (i === segs.length - 1) out.push(''); }
      else if (s === '.' || s.toLowerCase() === '%2e') { if (i === segs.length - 1) out.push(''); }
      else out.push(s);
    }
    let r = out.join('/');
    if (path.charAt(0) === '/' && r.charAt(0) !== '/') r = '/' + r;
    return r;
  }

  function encodePart(s, keep) {
    let out = '';
    for (const ch of s) {
      const c = ch.codePointAt(0);
      if (c < 0x21 || c > 0x7e || keep.indexOf(ch) < 0 && '"<>`{}'.indexOf(ch) >= 0 || ch === ' ') out += encodeURIComponent(ch);
      else out += ch;
    }
    return out;
  }

  function splitAuthority(auth, rec) {
    let host = auth;
    const at = host.lastIndexOf('@');
    rec.username = ''; rec.password = '';
    if (at >= 0) {
      const cred = host.slice(0, at);
      host = host.slice(at + 1);
      const colon = cred.indexOf(':');
      rec.username = colon >= 0 ? cred.slice(0, colon) : cred;
      rec.password = colon >= 0 ? cred.slice(colon + 1) : '';
    }
    let port = '';
    const m = /^(\[[^\]]*\]|[^:]*)(?::(\d*))?$/.exec(host);
    if (!m) throw new TypeError('Invalid URL');
    host = m[1].toLowerCase();
    port = m[2] || '';
    if (/[\s<>^|]/.test(host)) throw new TypeError('Invalid URL');
    if (port !== '') { port = String(parseInt(port, 10)); if (+port > 65535) throw new TypeError('Invalid URL'); }
    if (special[rec.protocol] === port) port = '';
    rec.hostname = host;
    rec.port = port;
  }

  function parse(input, base) {
    input = String(input).replace(/^[\u0000- ]+|[\u0000- ]+$/g, '').replace(/[\t\n\r]/g, '');
    const m = re.exec(input);
    if (!m) return null;
    let [, scheme, auth, path, query, frag] = m;
    const rec = { protocol: '', username: '', password: '', hostname: '', port: '', pathname: '', search: '', hash: '', opaque: false };
    if (scheme !== undefined) {
      rec.protocol = scheme.toLowerCase() + ':';
      const isSpecial = rec.protocol in special;
      if (isSpecial) path = path.replace(/\\/g, '/');
      if (auth === undefined && isSpecial && rec.protocol !== 'file:') {
        // "http:foo" — relative to a base of the same scheme, else its own authority.
        if (base && base.protocol === rec.protocol) return parse(input.slice(scheme.length + 1), base);
        const mm = /^\/*([^\/?#]*)(.*)$/.exec(path);
        auth = mm[1]; path = mm[2];
      }
      if (auth !== undefined) {
        splitAuthority(auth, rec);
        if (isSpecial && rec.protocol !== 'file:' && rec.hostname === '') return null;
        rec.pathname = removeDots(path || (isSpecial ? '/' : ''));
        if (isSpecial && rec.pathname === '') rec.pathname = '/';
      } else if (isSpecial) {
        rec.pathname = removeDots(path || '/');
      } else {
        rec.pathname = path;
        rec.opaque = path.charAt(0) !== '/';
      }
    } else {
      if (!base) return null;
      if (base.opaque && !(path === '' && query === undefined)) return null;
      rec.protocol = base.protocol;
      const isSpecial = rec.protocol in special;
      if (isSpecial) path = path.replace(/\\/g, '/');
      if (auth !== undefined) {
        splitAuthority(auth, rec);
        rec.pathname = removeDots(path || '/');
      } else {
        rec.username = base.username; rec.password = base.password; rec.hostname = base.hostname; rec.port = base.port;
        if (path === '') {
          rec.pathname = base.pathname;
          rec.opaque = base.opaque;
          if (query === undefined) query = base.search ? base.search.slice(1) : undefined;
        } else if (path.charAt(0) === '/') {
          rec.pathname = removeDots(path);
        } else {
          const dir = base.pathname.slice(0, base.pathname.lastIndexOf('/') + 1);
          rec.pathname = removeDots((dir || '/') + path);
        }
      }
    }
    if (!rec.opaque) rec.pathname = encodePart(rec.pathname, '');
    rec.search = query ? '?' + encodePart(query, '') : '';
    rec.hash = frag ? '#' + encodePart(frag, '') : '';
    return rec;
  }

  function hostOf(r) { return r.hostname + (r.port ? ':' + r.port : ''); }
  function hrefOf(r) {
    let s = r.protocol;
    if (r.hostname !== '' || r.protocol === 'file:' || (r.protocol in special)) {
      s += '//';
      if (r.username || r.password) s += r.username + (r.password ? ':' + r.password : '') + '@';
      s += hostOf(r);
    }
    return s + r.pathname + r.search + r.hash;
  }

  class URLSearchParams {
    constructor(init) {
      this[params] = [];
      this[owner] = null;
      if (init === undefined || init === null) return;
      if (typeof init === 'object' || typeof init === 'function') {
        if (typeof init[Symbol.iterator] === 'function') {
          for (const pair of init) {
            const p = Array.from(pair);
            if (p.length !== 2) throw new TypeError('each pair needs a name and a value');
            this[params].push([String(p[0]), String(p[1])]);
          }
        } else {
          for (const k of Object.keys(init)) this[params].push([k, String(init[k])]);
        }
      } else {
        this._parse(String(init));
      }
    }
    _parse(s) {
      this[params] = [];
      if (s.charAt(0) === '?') s = s.slice(1);
      for (const part of s.split('&')) {
        if (!part) continue;
        const eq = part.indexOf('=');
        const dec = x => { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch (e) { return x; } };
        this[params].push(eq >= 0 ? [dec(part.slice(0, eq)), dec(part.slice(eq + 1))] : [dec(part), '']);
      }
    }
    _update() { if (this[owner]) { const s = this.toString(); this[owner][state].search = s ? '?' + s : ''; } }
    get size() { return this[params].length; }
    append(k, v) { this[params].push([String(k), String(v)]); this._update(); }
    delete(k, v) { k = String(k); this[params] = this[params].filter(p => !(p[0] === k && (v === undefined || p[1] === String(v)))); this._update(); }
    get(k) { k = String(k); const p = this[params].find(p => p[0] === k); return p ? p[1] : null; }
    getAll(k) { k = String(k); return this[params].filter(p => p[0] === k).map(p => p[1]); }
    has(k, v) { k = String(k); return this[params].some(p => p[0] === k && (v === undefined || p[1] === String(v))); }
    set(k, v) {
      k = String(k); v = String(v);
      const i = this[params].findIndex(p => p[0] === k);
      if (i < 0) this[params].push([k, v]);
      else { this[params][i][1] = v; this[params] = this[params].filter((p, j) => j <= i || p[0] !== k); }
      this._update();
    }
    sort() { this[params] = this[params].map((p, i) => [p, i]).sort((a, b) => a[0][0] < b[0][0] ? -1 : a[0][0] > b[0][0] ? 1 : a[1] - b[1]).map(x => x[0]); this._update(); }
    forEach(fn, thisArg) { for (const [k, v] of this[params]) fn.call(thisArg, v, k, this); }
    keys() { return this[params].map(p => p[0])[Symbol.iterator](); }
    values() { return this[params].map(p => p[1])[Symbol.iterator](); }
    entries() { return this[params].map(p => [p[0], p[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() {
      const enc = s => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
      return this[params].map(p => enc(p[0]) + '=' + enc(p[1])).join('&');
    }
    get [Symbol.toStringTag]() { return 'URLSearchParams'; }
  }

  class URL {
    constructor(url, base) {
      let b = null;
      if (base !== undefined) {
        b = base instanceof URL ? base[state] : parse(String(base), null);
        if (!b) throw new TypeError('Invalid base URL: ' + base);
      }
      const r = parse(url instanceof URL ? url.href : String(url), b);
      if (!r) throw new TypeError('Invalid URL: ' + url);
      this[state] = r;
      this[params] = null;
    }
    static canParse(url, base) { try { new URL(url, base); return true; } catch (e) { return false; } }
    static parse(url, base) { try { return new URL(url, base); } catch (e) { return null; } }
    get href() { return hrefOf(this[state]); }
    set href(v) { const r = parse(String(v), null); if (!r) throw new TypeError('Invalid URL: ' + v); this[state] = r; if (this[params]) this[params]._parse(r.search); }
    get origin() { const r = this[state]; return (r.protocol in special) && r.protocol !== 'file:' ? r.protocol + '//' + hostOf(r) : 'null'; }
    get protocol() { return this[state].protocol; }
    set protocol(v) { v = String(v).replace(/:.*$/, '').toLowerCase(); if (/^[a-z][a-z0-9+.\-]*$/.test(v)) this[state].protocol = v + ':'; }
    get username() { return this[state].username; }
    set username(v) { this[state].username = encodePart(String(v), ''); }
    get password() { return this[state].password; }
    set password(v) { this[state].password = encodePart(String(v), ''); }
    get host() { return hostOf(this[state]); }
    set host(v) { const r = this[state]; try { splitAuthority(String(v), r); } catch (e) {} }
    get hostname() { return this[state].hostname; }
    set hostname(v) { this[state].hostname = String(v).toLowerCase(); }
    get port() { return this[state].port; }
    set port(v) { v = String(v); const r = this[state]; if (v === '') r.port = ''; else if (/^\d+/.test(v)) { const p = String(parseInt(v, 10)); r.port = special[r.protocol] === p ? '' : p; } }
    get pathname() { return this[state].pathname; }
    set pathname(v) { const r = this[state]; if (r.opaque) return; v = String(v); r.pathname = encodePart(removeDots(v.charAt(0) === '/' ? v : '/' + v), ''); }
    get search() { const s = this[state].search; return s === '?' ? '' : s; }
    set search(v) { v = String(v); if (v.charAt(0) === '?') v = v.slice(1); this[state].search = v ? '?' + encodePart(v, '') : ''; if (this[params]) this[params]._parse(v); }
    get searchParams() {
      if (!this[params]) { this[params] = new URLSearchParams(this[state].search); this[params][owner] = this; }
      return this[params];
    }
    get hash() { const h = this[state].hash; return h === '#' ? '' : h; }
    set hash(v) { v = String(v); if (v.charAt(0) === '#') v = v.slice(1); this[state].hash = v ? '#' + encodePart(v, '') : ''; }
    toString() { return this.href; }
    toJSON() { return this.href; }
    get [Symbol.toStringTag]() { return 'URL'; }
  }

  // AbortController and AbortSignal.
  const signalState = Symbol('signal');
  function domException(message, name) {
    const e = new Error(message);
    e.name = name;
    return e;
  }
  class AbortSignal extends EventTarget {
    constructor(key) {
      if (key !== signalState) throw new TypeError('Illegal constructor');
      super();
      this[signalState] = { aborted: false, reason: undefined };
      this.onabort = null;
    }
    get aborted() { return this[signalState].aborted; }
    get reason() { return this[signalState].reason; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    _abort(reason) {
      const st = this[signalState];
      if (st.aborted) return;
      st.aborted = true;
      st.reason = reason === undefined ? domException('signal is aborted without reason', 'AbortError') : reason;
      const e = new Event('abort');
      if (typeof this.onabort === 'function') { try { this.onabort.call(this, e); } catch (err) { console.error(err); } }
      this.dispatchEvent(e);
    }
    static abort(reason) { const s = new AbortSignal(signalState); s._abort(reason); return s; }
    static timeout(ms) {
      const s = new AbortSignal(signalState);
      setTimeout(() => s._abort(domException('signal timed out', 'TimeoutError')), ms);
      return s;
    }
    static any(signals) {
      const s = new AbortSignal(signalState);
      for (const o of signals) {
        if (o.aborted) { s._abort(o.reason); return s; }
        o.addEventListener('abort', () => s._abort(o.reason));
      }
      return s;
    }
    get [Symbol.toStringTag]() { return 'AbortSignal'; }
  }
  class AbortController {
    constructor() { this._signal = new AbortSignal(signalState); }
    get signal() { return this._signal; }
    abort(reason) { this._signal._abort(reason); }
    get [Symbol.toStringTag]() { return 'AbortController'; }
  }

  // NodeFilter, TreeWalker and NodeIterator, over the tree's own links.
  const NodeFilter = {
    FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3,
    SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 0x1, SHOW_ATTRIBUTE: 0x2, SHOW_TEXT: 0x4, SHOW_CDATA_SECTION: 0x8,
    SHOW_PROCESSING_INSTRUCTION: 0x40, SHOW_COMMENT: 0x80, SHOW_DOCUMENT: 0x100, SHOW_DOCUMENT_TYPE: 0x200,
    SHOW_DOCUMENT_FRAGMENT: 0x400,
  };
  function acceptNode(walker, node) {
    if (!((1 << (node.nodeType - 1)) & walker.whatToShow)) return NodeFilter.FILTER_SKIP;
    const f = walker.filter;
    if (!f) return NodeFilter.FILTER_ACCEPT;
    return typeof f === 'function' ? f(node) : f.acceptNode(node);
  }
  class TreeWalker {
    constructor(root, whatToShow, filter) {
      this.root = root; this.whatToShow = whatToShow >>> 0; this.filter = filter || null; this.currentNode = root;
    }
    parentNode() {
      let n = this.currentNode;
      while (n && n !== this.root) {
        n = n.parentNode;
        if (n && acceptNode(this, n) === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
      }
      return null;
    }
    _child(first) {
      let n = first ? this.currentNode.firstChild : this.currentNode.lastChild;
      while (n) {
        const r = acceptNode(this, n);
        if (r === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
        if (r === NodeFilter.FILTER_SKIP) {
          const c = first ? n.firstChild : n.lastChild;
          if (c) { n = c; continue; }
        }
        while (n) {
          const sib = first ? n.nextSibling : n.previousSibling;
          if (sib) { n = sib; break; }
          const p = n.parentNode;
          if (!p || p === this.root || p === this.currentNode) return null;
          n = p;
        }
      }
      return null;
    }
    firstChild() { return this._child(true); }
    lastChild() { return this._child(false); }
    _sibling(next) {
      let n = this.currentNode;
      if (n === this.root) return null;
      while (true) {
        let sib = next ? n.nextSibling : n.previousSibling;
        while (sib) {
          n = sib;
          const r = acceptNode(this, n);
          if (r === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
          sib = next ? n.firstChild : n.lastChild;
          if (r === NodeFilter.FILTER_REJECT || !sib) sib = next ? n.nextSibling : n.previousSibling;
        }
        n = n.parentNode;
        if (!n || n === this.root) return null;
        if (acceptNode(this, n) === NodeFilter.FILTER_ACCEPT) return null;
      }
    }
    nextSibling() { return this._sibling(true); }
    previousSibling() { return this._sibling(false); }
    nextNode() {
      let n = this.currentNode;
      let r = NodeFilter.FILTER_ACCEPT;
      while (true) {
        while (r !== NodeFilter.FILTER_REJECT && n.firstChild) {
          n = n.firstChild;
          r = acceptNode(this, n);
          if (r === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
        }
        let sib = null;
        let t = n;
        while (t) {
          if (t === this.root) return null;
          sib = t.nextSibling;
          if (sib) break;
          t = t.parentNode;
        }
        if (!sib) return null;
        n = sib;
        r = acceptNode(this, n);
        if (r === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
      }
    }
    previousNode() {
      let n = this.currentNode;
      while (n !== this.root) {
        let sib = n.previousSibling;
        while (sib) {
          n = sib;
          let r = acceptNode(this, n);
          while (r !== NodeFilter.FILTER_REJECT && n.lastChild) {
            n = n.lastChild;
            r = acceptNode(this, n);
          }
          if (r === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
          sib = n.previousSibling;
        }
        if (n === this.root || !n.parentNode) return null;
        n = n.parentNode;
        if (acceptNode(this, n) === NodeFilter.FILTER_ACCEPT) return this.currentNode = n;
      }
      return null;
    }
  }
  class NodeIterator {
    constructor(root, whatToShow, filter) {
      this._walker = new TreeWalker(root, whatToShow, filter);
      this.root = root; this.whatToShow = whatToShow >>> 0; this.filter = filter || null;
      this._started = false;
    }
    get referenceNode() { return this._walker.currentNode; }
    nextNode() {
      if (!this._started) {
        this._started = true;
        if (acceptNode(this._walker, this.root) === NodeFilter.FILTER_ACCEPT) return this.root;
      }
      return this._walker.nextNode();
    }
    previousNode() { return this._walker.previousNode(); }
    detach() {}
  }
  if (global.document) {
    const docProto = Object.getPrototypeOf(global.document);
    docProto.createTreeWalker = function (root, whatToShow = 0xFFFFFFFF, filter = null) { return new TreeWalker(root, whatToShow, filter); };
    docProto.createNodeIterator = function (root, whatToShow = 0xFFFFFFFF, filter = null) { return new NodeIterator(root, whatToShow, filter); };
  }

  for (const [name, value] of [['URL', URL], ['URLSearchParams', URLSearchParams], ['AbortController', AbortController],
                               ['AbortSignal', AbortSignal], ['NodeFilter', NodeFilter], ['TreeWalker', TreeWalker],
                               ['NodeIterator', NodeIterator]])
    Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
})(this);
)JS";

}  // namespace fbx::xweb
