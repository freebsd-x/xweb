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

  // Blob, FormData, Headers, Request, Response, fetch and XMLHttpRequest, over the
  // browser's __xwebFetch.
  const native = global.__xwebFetch;
  delete global.__xwebFetch;
  const bytes = Symbol('bytes');
  function toBytes(part) {
    if (part instanceof Blob) return part[bytes];
    if (part instanceof ArrayBuffer) return new Uint8Array(part.slice(0));
    if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer.slice(part.byteOffset, part.byteOffset + part.byteLength));
    return new TextEncoder().encode(String(part));
  }
  class Blob {
    constructor(parts = [], options = {}) {
      const chunks = Array.from(parts, toBytes);
      const out = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
      let at = 0;
      for (const c of chunks) { out.set(c, at); at += c.length; }
      this[bytes] = out;
      this.type = options.type ? String(options.type).toLowerCase() : '';
    }
    get size() { return this[bytes].length; }
    slice(start, end, type) { const b = new Blob([], { type }); b[bytes] = this[bytes].slice(start, end); return b; }
    text() { return Promise.resolve(new TextDecoder().decode(this[bytes])); }
    arrayBuffer() { return Promise.resolve(this[bytes].slice().buffer); }
    bytes() { return Promise.resolve(this[bytes].slice()); }
    get [Symbol.toStringTag]() { return 'Blob'; }
  }
  class File extends Blob {
    constructor(parts, name, options = {}) { super(parts, options); this.name = String(name); this.lastModified = options.lastModified ?? Date.now(); }
    get [Symbol.toStringTag]() { return 'File'; }
  }
  class FormData {
    constructor(form) {
      this._entries = [];
      if (form && form.elements) {
        for (const el of Array.from(form.elements)) {
          if (!el.name || el.disabled) continue;
          const type = (el.type || '').toLowerCase();
          if ((type === 'checkbox' || type === 'radio') && !el.checked) continue;
          if (type === 'submit' || type === 'button' || type === 'reset' || type === 'file') continue;
          this._entries.push([el.name, String(el.value ?? '')]);
        }
      }
    }
    append(k, v) { this._entries.push([String(k), v instanceof Blob ? v : String(v)]); }
    delete(k) { k = String(k); this._entries = this._entries.filter(e => e[0] !== k); }
    get(k) { k = String(k); const e = this._entries.find(e => e[0] === k); return e ? e[1] : null; }
    getAll(k) { k = String(k); return this._entries.filter(e => e[0] === k).map(e => e[1]); }
    has(k) { k = String(k); return this._entries.some(e => e[0] === k); }
    set(k, v) { this.delete(k); this.append(k, v); }
    forEach(fn, thisArg) { for (const [k, v] of this._entries) fn.call(thisArg, v, k, this); }
    keys() { return this._entries.map(e => e[0])[Symbol.iterator](); }
    values() { return this._entries.map(e => e[1])[Symbol.iterator](); }
    entries() { return this._entries.map(e => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return 'FormData'; }
  }
  class Headers {
    constructor(init) {
      this._list = [];
      if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
      else if (init && typeof init[Symbol.iterator] === 'function') for (const [k, v] of init) this.append(k, v);
      else if (init && typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { this._list.push([String(k).toLowerCase(), String(v).trim()]); }
    delete(k) { k = String(k).toLowerCase(); this._list = this._list.filter(e => e[0] !== k); }
    get(k) { k = String(k).toLowerCase(); const v = this._list.filter(e => e[0] === k).map(e => e[1]); return v.length ? v.join(', ') : null; }
    has(k) { k = String(k).toLowerCase(); return this._list.some(e => e[0] === k); }
    set(k, v) { this.delete(k); this.append(k, v); }
    getSetCookie() { return []; }
    _sorted() {
      const names = [...new Set(this._list.map(e => e[0]))].sort();
      return names.map(n => [n, this.get(n)]);
    }
    forEach(fn, thisArg) { for (const [k, v] of this._sorted()) fn.call(thisArg, v, k, this); }
    keys() { return this._sorted().map(e => e[0])[Symbol.iterator](); }
    values() { return this._sorted().map(e => e[1])[Symbol.iterator](); }
    entries() { return this._sorted()[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return 'Headers'; }
  }
  // A body and the content type it implies.
  function extractBody(body) {
    if (body === undefined || body === null) return [null, null];
    if (typeof body === 'string') return [body, 'text/plain;charset=UTF-8'];
    if (body instanceof URLSearchParams) return [body.toString(), 'application/x-www-form-urlencoded;charset=UTF-8'];
    if (body instanceof Blob) return [body[bytes].slice().buffer, body.type || null];
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return [body, null];
    if (body instanceof FormData) {
      const boundary = '----xweb' + Math.random().toString(16).slice(2) + Date.now().toString(16);
      let out = '';
      for (const [k, v] of body._entries) {
        const name = k.replace(/"/g, '%22');
        if (v instanceof Blob) {
          out += '--' + boundary + '\r\nContent-Disposition: form-data; name="' + name + '"; filename="' + (v.name || 'blob').replace(/"/g, '%22') + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n' + new TextDecoder('latin1').decode(v[bytes]) + '\r\n';
        } else {
          out += '--' + boundary + '\r\nContent-Disposition: form-data; name="' + name + '"\r\n\r\n' + v + '\r\n';
        }
      }
      out += '--' + boundary + '--\r\n';
      return [out, 'multipart/form-data; boundary=' + boundary];
    }
    return [String(body), 'text/plain;charset=UTF-8'];
  }
  const bodyData = Symbol('body');
  class Body {
    get bodyUsed() { return this._used === true; }
    _take() {
      if (this._used) return Promise.reject(new TypeError('the body has already been read'));
      this._used = true;
      return Promise.resolve(this[bodyData] || new Uint8Array(0));
    }
    arrayBuffer() { return this._take().then(b => b.slice().buffer); }
    bytes() { return this._take().then(b => b.slice()); }
    text() { return this._take().then(b => new TextDecoder().decode(b)); }
    json() { return this.text().then(t => JSON.parse(t)); }
    blob() { return this._take().then(b => { const x = new Blob([], { type: this.headers.get('content-type') || '' }); x[bytes] = b.slice(); return x; }); }
    formData() { return this.text().then(t => { const f = new FormData(); for (const [k, v] of new URLSearchParams(t)) f.append(k, v); return f; }); }
  }
  class Request extends Body {
    constructor(input, init = {}) {
      super();
      const from = input instanceof Request ? input : null;
      this.url = from ? from.url : new URL(String(input), document.baseURI).href;
      this.method = String(init.method || (from && from.method) || 'GET').toUpperCase();
      this.headers = new Headers(init.headers || (from && from.headers) || undefined);
      this.signal = init.signal || (from && from.signal) || null;
      this.credentials = init.credentials || 'same-origin';
      this.mode = init.mode || 'cors';
      this.cache = init.cache || 'default';
      this.redirect = init.redirect || 'follow';
      this.referrer = 'about:client';
      const [b, type] = extractBody(init.body !== undefined ? init.body : from ? from._source : undefined);
      if (b !== null && (this.method === 'GET' || this.method === 'HEAD')) throw new TypeError('a GET or HEAD request can\'t have a body');
      this._source = b;
      if (type && !this.headers.has('content-type')) this.headers.set('content-type', type);
      this[bodyData] = b === null ? null : toBytes(b);
    }
    clone() { return new Request(this); }
    get [Symbol.toStringTag]() { return 'Request'; }
  }
  const statusTexts = { 200: 'OK', 201: 'Created', 204: 'No Content', 301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified',
                        400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found', 500: 'Internal Server Error' };
  class Response extends Body {
    constructor(body = null, init = {}) {
      super();
      const status = init.status === undefined ? 200 : Number(init.status);
      if (status < 200 || status > 599) throw new RangeError('a status must be from 200 to 599');
      this.status = status;
      this.statusText = init.statusText !== undefined ? String(init.statusText) : '';
      this.headers = new Headers(init.headers);
      const [b, type] = extractBody(body);
      if (type && !this.headers.has('content-type')) this.headers.set('content-type', type);
      this[bodyData] = b === null ? null : toBytes(b);
      this.url = ''; this.redirected = false; this.type = 'default';
    }
    get ok() { return this.status >= 200 && this.status < 300; }
    clone() { if (this._used) throw new TypeError('the body has already been read'); const r = new Response(null, this); r[bodyData] = this[bodyData]; r.url = this.url; r.type = this.type; r.redirected = this.redirected; return r; }
    static json(data, init = {}) { const h = new Headers(init.headers); if (!h.has('content-type')) h.set('content-type', 'application/json'); return new Response(JSON.stringify(data), { ...init, headers: h }); }
    static error() { const r = new Response(null, { status: 200 }); r.status = 0; r.type = 'error'; return r; }
    static redirect(url, status = 302) { return new Response(null, { status, headers: { location: new URL(url, document.baseURI).href } }); }
    get [Symbol.toStringTag]() { return 'Response'; }
  }
  function abortError(signal) { return signal.reason !== undefined ? signal.reason : domException('The operation was aborted.', 'AbortError'); }
  function fetch(input, init) {
    let req;
    try { req = new Request(input, init); } catch (e) { return Promise.reject(e); }
    if (req.signal && req.signal.aborted) return Promise.reject(abortError(req.signal));
    const done = native(req.url, req.method, req.headers.get('content-type'), req._source).then(([status, url, type, buf]) => {
      if (req.signal && req.signal.aborted) throw abortError(req.signal);
      const res = new Response(null, { status: status >= 200 && status <= 599 ? status : 200, statusText: statusTexts[status] || '', headers: type ? { 'content-type': type } : {} });
      res.status = status;
      res[bodyData] = new Uint8Array(buf);
      res.url = url; res.redirected = url !== req.url.split('#')[0]; res.type = 'basic';
      return res;
    });
    if (!req.signal) return done;
    return new Promise((resolve, reject) => {
      req.signal.addEventListener('abort', () => reject(abortError(req.signal)));
      done.then(resolve, reject);
    });
  }
  class XMLHttpRequest extends EventTarget {
    constructor() {
      super();
      this.readyState = 0; this.status = 0; this.statusText = ''; this.responseType = ''; this.responseURL = '';
      this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget();
      this._headers = new Headers(); this._response = null; this._type = '';
      for (const n of ['onreadystatechange', 'onload', 'onerror', 'onabort', 'onloadend', 'onloadstart', 'onprogress', 'ontimeout']) this[n] = null;
    }
    _fire(type) {
      const e = new Event(type);
      const h = this['on' + type];
      if (typeof h === 'function') { try { h.call(this, e); } catch (err) { console.error(err); } }
      this.dispatchEvent(e);
    }
    _state(n) { this.readyState = n; this._fire('readystatechange'); }
    open(method, url, async = true) {
      this._method = String(method).toUpperCase(); this._url = new URL(String(url), document.baseURI).href;
      this._headers = new Headers(); this._aborted = false; this._response = null; this.status = 0;
      this._state(1);
    }
    setRequestHeader(k, v) { if (this.readyState !== 1) throw new Error('InvalidStateError'); this._headers.append(k, v); }
    getResponseHeader(k) { return String(k).toLowerCase() === 'content-type' && this._type ? this._type : null; }
    getAllResponseHeaders() { return this._type ? 'content-type: ' + this._type + '\r\n' : ''; }
    overrideMimeType(t) { this._override = t; }
    send(body) {
      if (this.readyState !== 1) throw new Error('InvalidStateError');
      const [b, type] = extractBody(this._method === 'GET' || this._method === 'HEAD' ? null : body);
      this._fire('loadstart');
      native(this._url, this._method, this._headers.get('content-type') || type, b).then(([status, url, ctype, buf]) => {
        if (this._aborted) return;
        this.status = status; this.statusText = statusTexts[status] || ''; this.responseURL = url; this._type = ctype;
        this._bytes = new Uint8Array(buf);
        this._state(2); this._state(3); this._state(4);
        this._fire('load'); this._fire('loadend');
      }, () => {
        if (this._aborted) return;
        this._state(4); this._fire('error'); this._fire('loadend');
      });
    }
    abort() { this._aborted = true; if (this.readyState > 0 && this.readyState < 4) { this._state(4); this._fire('abort'); this._fire('loadend'); } this.readyState = 0; }
    get responseText() {
      if (this.responseType !== '' && this.responseType !== 'text') throw new Error('InvalidStateError');
      return this._bytes ? new TextDecoder().decode(this._bytes) : '';
    }
    get response() {
      if (!this._bytes) return this.responseType === '' || this.responseType === 'text' ? '' : null;
      switch (this.responseType) {
        case 'json': try { return JSON.parse(new TextDecoder().decode(this._bytes)); } catch (e) { return null; }
        case 'arraybuffer': return this._bytes.slice().buffer;
        case 'blob': { const b = new Blob([], { type: this._type }); b[bytes] = this._bytes.slice(); return b; }
        case 'document': return null;
        default: return new TextDecoder().decode(this._bytes);
      }
    }
    get responseXML() { return null; }
  }
  for (const [k, v] of [['UNSENT', 0], ['OPENED', 1], ['HEADERS_RECEIVED', 2], ['LOADING', 3], ['DONE', 4]]) {
    XMLHttpRequest[k] = v; XMLHttpRequest.prototype[k] = v;
  }

  for (const [name, value] of [['Blob', Blob], ['File', File], ['FormData', FormData], ['Headers', Headers], ['Request', Request],
                               ['Response', Response], ['fetch', fetch], ['XMLHttpRequest', XMLHttpRequest]])
    Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
  for (const [name, value] of [['URL', URL], ['URLSearchParams', URLSearchParams], ['AbortController', AbortController],
                               ['AbortSignal', AbortSignal], ['NodeFilter', NodeFilter], ['TreeWalker', TreeWalker],
                               ['NodeIterator', NodeIterator]])
    Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
})(this);
)JS";

}  // namespace fbx::xweb
