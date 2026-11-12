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

  // DOMException, with its legacy codes.
  const exceptionCodes = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
    NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
    InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18,
    NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23,
    InvalidNodeTypeError: 24, DataCloneError: 25 };
  class DOMException extends Error {
    constructor(message = '', name = 'Error') {
      super(String(message));
      Object.defineProperty(this, 'name', { value: String(name), writable: true, configurable: true });
      Object.defineProperty(this, 'code', { value: exceptionCodes[name] || 0, configurable: true });
    }
    get [Symbol.toStringTag]() { return 'DOMException'; }
  }
  for (const [name, code] of Object.entries(exceptionCodes)) {
    const constant = name.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR');
    DOMException[constant] = code; DOMException.prototype[constant] = code;
  }

  // AbortController and AbortSignal.
  const signalState = Symbol('signal');
  function domException(message, name) { return new DOMException(message, name); }
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
      this.dispatchEvent(new Event('abort'));   // (its onabort too)
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
  // blob: URLs for Blobs, which fetch() answers from here until they're revoked.
  const blobUrls = new Map();
  URL.createObjectURL = function createObjectURL(obj) {
    if (!(obj instanceof Blob)) throw new TypeError("createObjectURL takes a Blob");
    let id = '';
    for (let i = 0; i < 32; i++) id += '0123456789abcdef'[Math.floor(Math.random() * 16)];
    const url = 'blob:' + (typeof location === 'object' && location.origin ? location.origin : 'null') + '/' +
      id.slice(0, 8) + '-' + id.slice(8, 12) + '-' + id.slice(12, 16) + '-' + id.slice(16, 20) + '-' + id.slice(20);
    if (blobUrls.size < 10000) blobUrls.set(url, obj);
    return url;
  };
  URL.revokeObjectURL = function revokeObjectURL(url) { blobUrls.delete(String(url)); };
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
    // The body as a stream (Next.js reads its responses with body.getReader()): all of it in
    // one chunk, the body used once it's read.
    get body() {
      if (this[bodyData] === null || this[bodyData] === undefined) return null;
      if (!this._stream) {
        const self = this;
        this._stream = new ReadableStream({
          pull(c) {
            const data = self[bodyData] || new Uint8Array(0);
            self._used = true;
            if (data.length) c.enqueue(data.slice());
            c.close();
          },
        }, { highWaterMark: 0 });
      }
      return this._stream;
    }
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
    if (req.url.startsWith('blob:')) {
      const blob = blobUrls.get(req.url.split('#')[0]);
      if (!blob || req.method !== 'GET') return Promise.reject(new TypeError('Failed to fetch'));
      const res = new Response(null, { status: 200, headers: blob.type ? { 'content-type': blob.type } : {} });
      res[bodyData] = blob[bytes].slice();
      res.url = req.url;
      return Promise.resolve(res);
    }
    const done = native(req.url, req.method, [...req.headers], req._source, req.mode, req.credentials).then(([status, url, type, buf, headers, kind]) => {
      if (req.signal && req.signal.aborted) throw abortError(req.signal);
      const res = new Response(null, { status: 200, headers });
      res.status = status;
      res.statusText = statusTexts[status] || '';
      res[bodyData] = new Uint8Array(buf);
      res.url = url; res.redirected = kind !== 'opaque' && url !== req.url.split('#')[0]; res.type = kind;
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
    // (dispatchEvent calls the on<type> property too: calling it here as well ran
    // Angular's onload twice, the second time after it had let go of the request.)
    _fire(type) { this.dispatchEvent(new Event(type)); }
    _state(n) { this.readyState = n; this._fire('readystatechange'); }
    open(method, url, async = true) {
      this._method = String(method).toUpperCase(); this._url = new URL(String(url), document.baseURI).href;
      this._headers = new Headers(); this._aborted = false; this._response = null; this.status = 0;
      this._state(1);
    }
    setRequestHeader(k, v) { if (this.readyState !== 1) throw new Error('InvalidStateError'); this._headers.append(k, v); }
    getResponseHeader(k) { return this._responseHeaders ? this._responseHeaders.get(k) : null; }
    getAllResponseHeaders() { return this._responseHeaders ? [...this._responseHeaders].map(([k, v]) => k + ': ' + v + '\r\n').join('') : ''; }
    overrideMimeType(t) { this._override = t; }
    send(body) {
      if (this.readyState !== 1) throw new Error('InvalidStateError');
      const [b, type] = extractBody(this._method === 'GET' || this._method === 'HEAD' ? null : body);
      this._fire('loadstart');
      if (type && !this._headers.has('content-type')) this._headers.set('content-type', type);
      native(this._url, this._method, [...this._headers], b, 'cors', this.withCredentials ? 'include' : 'same-origin').then(([status, url, ctype, buf, headers]) => {
        if (this._aborted) return;
        this.status = status; this.statusText = statusTexts[status] || ''; this.responseURL = url; this._type = ctype;
        this._responseHeaders = new Headers(headers);
        this._bytes = new Uint8Array(buf);
        this._state(2); this._state(3); this._state(4);
        this._fire('load'); this._fire('loadend');
      }, () => {
        if (this._aborted) return;
        this._state(4); this._fire('error'); this._fire('loadend');
      });
    }
    abort() { this._aborted = true; if (this.readyState > 0 && this.readyState < 4) { this._state(4); this._fire('abort'); this._fire('loadend'); } this.readyState = 0; }
    // Text in the charset the response (or overrideMimeType) names: Nate's EUC-KR keywords.
    _text() {
      const m = /charset\s*=\s*"?([^";\s]+)/i.exec(this._override || this._type || '');
      try { return new TextDecoder(m ? m[1] : 'utf-8').decode(this._bytes); } catch (e) { return new TextDecoder().decode(this._bytes); }
    }
    get responseText() {
      if (this.responseType !== '' && this.responseType !== 'text') throw new Error('InvalidStateError');
      return this._bytes ? this._text() : '';
    }
    get response() {
      if (!this._bytes) return this.responseType === '' || this.responseType === 'text' ? '' : null;
      switch (this.responseType) {
        case 'json': try { return JSON.parse(new TextDecoder().decode(this._bytes)); } catch (e) { return null; }
        case 'arraybuffer': return this._bytes.slice().buffer;
        case 'blob': { const b = new Blob([], { type: this._type }); b[bytes] = this._bytes.slice(); return b; }
        case 'document': return null;
        default: return this._text();
      }
    }
    get responseXML() { return null; }
  }
  for (const [k, v] of [['UNSENT', 0], ['OPENED', 1], ['HEADERS_RECEIVED', 2], ['LOADING', 3], ['DONE', 4]]) {
    XMLHttpRequest[k] = v; XMLHttpRequest.prototype[k] = v;
  }

  // structuredClone: the structured clone algorithm's common cases, cycles included.
  function structuredClone(value, options) {
    const seen = new Map();
    const fail = what => { throw domException(what + ' could not be cloned.', 'DataCloneError'); };
    function clone(v) {
      if (v === null || (typeof v !== 'object' && typeof v !== 'function')) {
        if (typeof v === 'symbol') fail('A symbol');
        return v;
      }
      if (typeof v === 'function') fail('A function');
      if (seen.has(v)) return seen.get(v);
      let out;
      if (Array.isArray(v)) { out = new Array(v.length); seen.set(v, out); for (let i = 0; i < v.length; i++) if (i in v) out[i] = clone(v[i]); return out; }
      if (v instanceof Date) out = new Date(v.getTime());
      else if (v instanceof RegExp) out = new RegExp(v.source, v.flags);
      else if (v instanceof ArrayBuffer) out = v.slice(0);
      else if (ArrayBuffer.isView(v)) { const C = v.constructor; out = v instanceof DataView ? new DataView(v.buffer.slice(0), v.byteOffset, v.byteLength) : new C(v); }
      else if (v instanceof Map) { out = new Map(); seen.set(v, out); for (const [k, x] of v) out.set(clone(k), clone(x)); return out; }
      else if (v instanceof Set) { out = new Set(); seen.set(v, out); for (const x of v) out.add(clone(x)); return out; }
      else if (v instanceof Error) { out = new (global[v.name] || Error)(v.message); if ('cause' in v) out.cause = clone(v.cause); }
      else if (v instanceof Blob) out = v.slice(0, v.size, v.type);
      else if (v instanceof Boolean || v instanceof Number || v instanceof String) out = Object(v.valueOf());
      else if (typeof Node !== 'undefined' && v instanceof Node) fail('A DOM node');
      else if (v instanceof Promise || v instanceof WeakMap || v instanceof WeakSet) fail('This object');
      else { out = {}; seen.set(v, out); for (const k of Object.keys(v)) out[k] = clone(v[k]); return out; }
      seen.set(v, out);
      return out;
    }
    return clone(value);
  }

  // requestIdleCallback: soon, with time to spare.
  let idleId = 0;
  const idleTimers = new Map();
  function requestIdleCallback(cb, options) {
    const id = ++idleId;
    idleTimers.set(id, setTimeout(() => {
      idleTimers.delete(id);
      const end = Date.now() + 50;
      cb({ didTimeout: false, timeRemaining: () => Math.max(0, end - Date.now()) });
    }, 1));
    return id;
  }
  function cancelIdleCallback(id) { clearTimeout(idleTimers.get(id)); idleTimers.delete(id); }

  // IntersectionObserver and ResizeObserver: each target reported once after it's
  // observed, from its laid-out box (enough for lazy loading and layout code to go on).
  function entryRect(el) {
    const r = el.getBoundingClientRect();
    return { x: r.x, y: r.y, width: r.width, height: r.height, top: r.top, left: r.left, right: r.right, bottom: r.bottom };
  }
  // IntersectionObserver: each target's crossing in or out of the viewport, said when
  // it's first observed and again whenever it changes (scrolling, or the page moving).
  const liveObservers = new Set();
  let observerTimer = 0, observerTicks = 0;
  const checkObservers = () => { for (const o of Array.from(liveObservers)) o._check(false); };
  // Polled a while after something's observed, for pages that move as they settle; then
  // only scrolling and resizing look again, so an idle page stays idle.
  const keepChecking = (restart) => {
    if (restart) observerTicks = 0;
    if (observerTimer || !liveObservers.size || observerTicks >= 40) return;
    observerTimer = setTimeout(function tick() {
      observerTimer = 0;
      ++observerTicks;
      checkObservers();
      keepChecking(false);
    }, 250);
  };
  global.addEventListener('scroll', checkObservers);
  global.addEventListener('resize', checkObservers);
  class IntersectionObserver {
    constructor(callback, options = {}) {
      if (typeof callback !== 'function') throw new TypeError('IntersectionObserver needs a callback');
      this._cb = callback; this._targets = new Set(); this._state = new Map(); this._fresh = new Set();
      this.root = options.root || null; this.rootMargin = options.rootMargin || '0px';
      this.thresholds = [].concat(options.threshold ?? 0);
      const m = /^\s*(-?[\d.]+)px/.exec(this.rootMargin);
      this._margin = m ? Number(m[1]) : 0;
    }
    observe(el) {
      if (this._targets.has(el)) return;
      this._targets.add(el);
      this._fresh.add(el);
      liveObservers.add(this);
      setTimeout(() => this._check(true), 0);
      keepChecking(true);
    }
    _check(firstOnly) {
      if (!this._targets.size) return;
      const vh = global.innerHeight, vw = global.innerWidth, m = this._margin;
      const entries = [];
      for (const target of this._targets) {
        const fresh = this._fresh.has(target);
        if (firstOnly && !fresh) continue;
        const r = entryRect(target);
        const visible = (r.width > 0 || r.height > 0) && r.bottom > -m && r.top < vh + m && r.right > -m && r.left < vw + m;
        if (!fresh && this._state.get(target) === visible) continue;
        this._fresh.delete(target);
        this._state.set(target, visible);
        entries.push({ target, isIntersecting: visible, intersectionRatio: visible ? 1 : 0, boundingClientRect: r,
                       intersectionRect: visible ? r : { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 },
                       rootBounds: { x: 0, y: 0, width: vw, height: vh, top: 0, left: 0, right: vw, bottom: vh }, time: performance.now() });
      }
      if (entries.length) this._cb(entries, this);
    }
    unobserve(el) { this._targets.delete(el); this._state.delete(el); this._fresh.delete(el); if (!this._targets.size) liveObservers.delete(this); }
    disconnect() { this._targets.clear(); this._state.clear(); this._fresh.clear(); liveObservers.delete(this); }
    takeRecords() { return []; }
  }
  class ResizeObserver {
    constructor(callback) {
      if (typeof callback !== 'function') throw new TypeError('ResizeObserver needs a callback');
      this._cb = callback; this._targets = new Set(); this._queued = [];
    }
    observe(el) {
      if (this._targets.has(el)) return;
      this._targets.add(el);
      this._queued.push(el);
      if (this._queued.length === 1) setTimeout(() => this._flush(), 0);
    }
    _flush() {
      const els = this._queued.filter(e => this._targets.has(e)); this._queued = [];
      if (!els.length) return;
      const entries = els.map(target => {
        const r = entryRect(target);
        const size = [{ inlineSize: r.width, blockSize: r.height }];
        return { target, contentRect: r, borderBoxSize: size, contentBoxSize: size, devicePixelContentBoxSize: size };
      });
      this._cb(entries, this);
    }
    unobserve(el) { this._targets.delete(el); }
    disconnect() { this._targets.clear(); this._queued = []; }
  }

  // PerformanceObserver and the performance timeline: there are no entries to give, but
  // pages that ask get the shape they expect.
  class PerformanceObserver {
    constructor(callback) { if (typeof callback !== 'function') throw new TypeError('PerformanceObserver needs a callback'); this._cb = callback; }
    observe() {}
    disconnect() {}
    takeRecords() { return []; }
    static get supportedEntryTypes() { return []; }
  }
  if (global.performance) {
    const perf = global.performance;
    const marks = [];
    if (!perf.mark) perf.mark = (name, options = {}) => { const e = { name, entryType: 'mark', startTime: options.startTime ?? perf.now(), duration: 0, detail: options.detail ?? null }; marks.push(e); return e; };
    if (!perf.measure) perf.measure = (name, start, end) => {
      const at = n => typeof n === 'string' ? (marks.filter(m => m.name === n).pop() || { startTime: 0 }).startTime : (n ?? perf.now());
      const s0 = typeof start === 'object' && start ? at(start.start) : at(start ?? 0), s1 = typeof start === 'object' && start ? at(start.end) : at(end);
      const e = { name, entryType: 'measure', startTime: s0, duration: s1 - s0 }; marks.push(e); return e;
    };
    if (!perf.getEntries) perf.getEntries = () => marks.slice();
    if (!perf.getEntriesByType) perf.getEntriesByType = t => marks.filter(m => m.entryType === t);
    if (!perf.getEntriesByName) perf.getEntriesByName = (n, t) => marks.filter(m => m.name === n && (!t || m.entryType === t));
    if (!perf.clearMarks) perf.clearMarks = n => { for (let i = marks.length; i--;) if (marks[i].entryType === 'mark' && (n === undefined || marks[i].name === n)) marks.splice(i, 1); };
    if (!perf.clearMeasures) perf.clearMeasures = n => { for (let i = marks.length; i--;) if (marks[i].entryType === 'measure' && (n === undefined || marks[i].name === n)) marks.splice(i, 1); };
    if (!perf.timeOrigin) perf.timeOrigin = Date.now() - perf.now();
    // The page's own load, as a navigation entry and the older timing object. We don't
    // measure the network phases, so they all happen at the start.
    const origin = perf.timeOrigin;
    const nav = {
      name: String(location.href), entryType: 'navigation', initiatorType: 'navigation', startTime: 0, duration: 0, type: 'navigate',
      redirectCount: 0, nextHopProtocol: 'http/1.1', workerStart: 0, redirectStart: 0, redirectEnd: 0, fetchStart: 0, domainLookupStart: 0,
      domainLookupEnd: 0, connectStart: 0, secureConnectionStart: 0, connectEnd: 0, requestStart: 0, responseStart: 0, responseEnd: 0,
      transferSize: 0, encodedBodySize: 0, decodedBodySize: 0, serverTiming: [],
      domInteractive: 0, domContentLoadedEventStart: 0, domContentLoadedEventEnd: 0, domComplete: 0, loadEventStart: 0, loadEventEnd: 0,
      toJSON() { return Object.assign({}, this); },
    };
    document.addEventListener('DOMContentLoaded', () => { const t = perf.now(); nav.domInteractive = nav.domContentLoadedEventStart = nav.domContentLoadedEventEnd = t; });
    global.addEventListener('load', () => { const t = perf.now(); nav.domComplete = nav.loadEventStart = t; setTimeout(() => { nav.loadEventEnd = nav.duration = perf.now(); }, 0); });
    // The entry interfaces, for instanceof (Reddit checks PerformanceNavigationTiming).
    class PerformanceEntry { toJSON() { return Object.assign({}, this); } }
    class PerformanceResourceTiming extends PerformanceEntry {}
    class PerformanceNavigationTiming extends PerformanceResourceTiming {}
    class PerformanceMark extends PerformanceEntry {}
    class PerformanceMeasure extends PerformanceEntry {}
    class PerformancePaintTiming extends PerformanceEntry {}
    for (const [name, C] of Object.entries({ PerformanceEntry, PerformanceResourceTiming, PerformanceNavigationTiming, PerformanceMark,
                                             PerformanceMeasure, PerformancePaintTiming }))
      if (!global[name]) Object.defineProperty(global, name, { value: C, writable: true, configurable: true });
    Object.setPrototypeOf(nav, global.PerformanceNavigationTiming.prototype);
    const typed = (e) => {
      if (e && typeof e === 'object' && Object.getPrototypeOf(e) === Object.prototype)
        Object.setPrototypeOf(e, e.entryType === 'mark' ? global.PerformanceMark.prototype : e.entryType === 'measure' ? global.PerformanceMeasure.prototype : global.PerformanceEntry.prototype);
      return e;
    };
    const all = () => [nav].concat(marks.map(typed));
    perf.getEntries = () => all();
    perf.getEntriesByType = t => all().filter(m => m.entryType === t);
    perf.getEntriesByName = (n, t) => all().filter(m => m.name === n && (!t || m.entryType === t));
    // Resource timing: no entries are kept, so the buffer's size is only remembered.
    if (!perf.setResourceTimingBufferSize) perf.setResourceTimingBufferSize = () => {};
    if (!perf.clearResourceTimings) perf.clearResourceTimings = () => {};
    if (!perf.timing) {
      const timing = {};
      for (const k of ['navigationStart', 'unloadEventStart', 'unloadEventEnd', 'redirectStart', 'redirectEnd', 'fetchStart', 'domainLookupStart',
                       'domainLookupEnd', 'connectStart', 'connectEnd', 'secureConnectionStart', 'requestStart', 'responseStart', 'responseEnd',
                       'domLoading', 'domInteractive', 'domContentLoadedEventStart', 'domContentLoadedEventEnd', 'domComplete',
                       'loadEventStart', 'loadEventEnd'])
        Object.defineProperty(timing, k, { enumerable: true, get() {
          if (/^(unload|redirect)/.test(k)) return 0;                       // none happened
          if (/^(domInteractive|domContentLoaded|domComplete|loadEvent)/.test(k)) return nav[k] ? Math.round(origin + nav[k]) : 0;   // not yet: 0
          return Math.round(origin);                                        // the network phases, and domLoading
        } });
      timing.toJSON = function () { const o = {}; for (const k in this) if (k !== 'toJSON') o[k] = this[k]; return o; };
      perf.timing = timing;
    }
    if (!perf.navigation) perf.navigation = { type: 0, redirectCount: 0, TYPE_NAVIGATE: 0, TYPE_RELOAD: 1, TYPE_BACK_FORWARD: 2, TYPE_RESERVED: 255 };
  }

  // The legacy event initialisers, for events made by document.createEvent.
  if (typeof Event === 'function') {
    const init = (e, type, bubbles, cancelable) => {
      if (typeof e.initEvent === 'function') e.initEvent(type, bubbles, cancelable);
    };
    const set = (e, k, v) => { try { Object.defineProperty(e, k, { value: v, enumerable: true, configurable: true, writable: true }); } catch (x) {} };
    Event.prototype.initCustomEvent = function initCustomEvent(type, bubbles, cancelable, detail) {
      init(this, type, bubbles, cancelable); set(this, 'detail', detail === undefined ? null : detail);
    };
    Event.prototype.initUIEvent = function initUIEvent(type, bubbles, cancelable, view, detail) {
      init(this, type, bubbles, cancelable); set(this, 'view', view ?? null); set(this, 'detail', detail | 0);
    };
    Event.prototype.initMouseEvent = function initMouseEvent(type, bubbles, cancelable, view, detail, sx, sy, cx, cy, ctrl, alt, shift, meta, button, related) {
      init(this, type, bubbles, cancelable);
      const v = { view: view ?? null, detail: detail | 0, screenX: sx | 0, screenY: sy | 0, clientX: cx | 0, clientY: cy | 0, ctrlKey: !!ctrl,
                  altKey: !!alt, shiftKey: !!shift, metaKey: !!meta, button: button | 0, relatedTarget: related ?? null };
      for (const k in v) set(this, k, v[k]);
    };
    Event.prototype.initKeyboardEvent = function initKeyboardEvent(type, bubbles, cancelable, view, key, location, ctrl, alt, shift, meta) {
      init(this, type, bubbles, cancelable);
      const v = { view: view ?? null, key: key ?? '', location: location | 0, ctrlKey: !!ctrl, altKey: !!alt, shiftKey: !!shift, metaKey: !!meta };
      for (const k in v) set(this, k, v[k]);
    };
  }

  // element.dataset: live, a data-* attribute per camel-cased name.
  if (typeof HTMLElement === 'function' && typeof Proxy === 'function') {
    const sets = new WeakMap();
    const attrOf = (k) => 'data-' + String(k).replace(/[A-Z]/g, c => '-' + c.toLowerCase());
    const keyOf = (a) => a.slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase());
    // (Instagram asks `dataset instanceof DOMStringMap`.)
    class DOMStringMap { constructor() { throw new TypeError('Illegal constructor'); } }
    Object.defineProperty(global, 'DOMStringMap', { value: DOMStringMap, writable: true, configurable: true });
    const datasetOf = function () {
      const el = this;
      let d = sets.get(el);
      if (d) return d;
      d = new Proxy(Object.create(DOMStringMap.prototype), {
        get(_, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute(attrOf(k)); return v === null ? undefined : v; },
        set(_, k, v) { if (typeof k === 'string') el.setAttribute(attrOf(k), String(v)); return true; },
        has(_, k) { return typeof k === 'string' && el.hasAttribute(attrOf(k)); },
        deleteProperty(_, k) { if (typeof k === 'string') el.removeAttribute(attrOf(k)); return true; },
        ownKeys() { return Array.from(el.attributes).map(a => a.name).filter(n => n.startsWith('data-')).map(keyOf); },
        getOwnPropertyDescriptor(_, k) {
          if (typeof k !== 'string') return undefined;
          const v = el.getAttribute(attrOf(k));
          return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
        },
      });
      sets.set(el, d);
      return d;
    };
    Object.defineProperty(Element.prototype, 'dataset', { get: datasetOf, configurable: true, enumerable: true });
  }

  // The global's event handler properties, as null until set ('onsubmit' in window).
  for (const t of ['abort', 'blur', 'cancel', 'change', 'click', 'close', 'contextmenu', 'dblclick', 'drag', 'dragend', 'dragenter', 'dragleave',
                   'dragover', 'dragstart', 'drop', 'error', 'focus', 'input', 'invalid', 'keydown', 'keypress', 'keyup', 'load', 'mousedown',
                   'mouseenter', 'mouseleave', 'mousemove', 'mouseout', 'mouseover', 'mouseup', 'pointerdown', 'pointermove', 'pointerup',
                   'reset', 'resize', 'scroll', 'select', 'submit', 'toggle', 'wheel', 'beforeunload', 'hashchange', 'message', 'offline',
                   'online', 'pagehide', 'pageshow', 'popstate', 'storage', 'unload', 'unhandledrejection', 'animationend', 'transitionend'])
    if (!(('on' + t) in global)) global['on' + t] = null;

  // Live lists: childNodes, children and getElementsBy* follow the tree as it changes
  // (for (; list.length; ) frag.appendChild(list[0]) must end). Each is a view, kept for
  // its node, whose contents are looked up again once the tree has changed.
  if (global.__xwebDomVersion && typeof Node === 'function') {
    const version = global.__xwebDomVersion;
    delete global.__xwebDomVersion;
    const listProto = Object.getPrototypeOf(document.createElement('div').childNodes);
    const isIndex = (k) => typeof k === 'string' && k !== '' && String(k >>> 0) === k;
    const live = (compute) => {
      let cache = null, at = -1;
      const now = () => { const v = version(); if (!cache || v !== at) { cache = compute(); at = v; } return cache; };
      const target = Object.setPrototypeOf([], listProto);
      return new Proxy(target, {
        get(t, k, r) {
          if (k === 'length') return now().length;
          if (isIndex(k)) return now()[+k];
          return Reflect.get(t, k, r);
        },
        has(t, k) { return k === 'length' || (isIndex(k) ? +k < now().length : Reflect.has(t, k)); },
        ownKeys() { return Array.from(now().keys(), String).concat(['length']); },
        getOwnPropertyDescriptor(t, k) {
          if (k === 'length') return { value: now().length, writable: true, enumerable: false, configurable: false };
          if (isIndex(k) && +k < now().length) return { value: now()[+k], writable: false, enumerable: true, configurable: true };
          return undefined;
        },
        set() { return true; },
        defineProperty() { return false; },
        deleteProperty() { return false; },
      });
    };
    const kept = new WeakMap();
    const keptFor = (node, key, make) => {
      let m = kept.get(node);
      if (!m) { m = new Map(); kept.set(node, m); }
      let l = m.get(key);
      if (!l) { l = make(); if (m.size < 64) m.set(key, l); }
      return l;
    };
    const liveGetter = (proto, name) => {
      const d = Object.getOwnPropertyDescriptor(proto, name);
      if (!d || !d.get) return;
      const orig = d.get;
      Object.defineProperty(proto, name, { get() { const node = this; return keptFor(node, name, () => live(() => orig.call(node))); },
                                           configurable: true, enumerable: d.enumerable });
    };
    const liveMethod = (proto, name) => {
      const orig = proto[name];
      if (typeof orig !== 'function') return;
      proto[name] = function (...args) {
        const node = this;
        orig.apply(node, args);   // its own checks, and errors, first
        return keptFor(node, name + '\u0000' + args.map(String).join('\u0000'), () => live(() => orig.apply(node, args)));
      };
    };
    liveGetter(Node.prototype, 'childNodes');
    for (const P of [Element.prototype, Document.prototype, DocumentFragment.prototype]) {
      liveGetter(P, 'children');
      if (P !== DocumentFragment.prototype) { liveMethod(P, 'getElementsByTagName'); liveMethod(P, 'getElementsByClassName'); }
    }
    liveMethod(Document.prototype, 'getElementsByName');
  }

  // Streams (WHATWG): a ReadableStream with its default controller and reader, tee, pipes
  // and async iteration; WritableStream and TransformStream enough to pipe through. Chunks
  // are counted, not sized, unless a strategy says how.
  if (typeof global.ReadableStream !== 'function') {
    const S = Symbol('stream');
    class ReadableStreamDefaultController {
      constructor() { throw new TypeError('Illegal constructor'); }
      get desiredSize() { const st = this[S]; return st.state === 'errored' ? null : st.state === 'closed' ? 0 : st.hwm - st.queueSize; }
      enqueue(chunk) {
        const st = this[S];
        if (st.closeRequested || st.state !== 'readable') throw new TypeError('the stream is closed or errored');
        if (st.reads.length) { st.reads.shift().resolve({ value: chunk, done: false }); }
        else { const size = st.size ? Number(st.size(chunk)) : 1; st.queue.push([chunk, size]); st.queueSize += size; }
        pull(st);
      }
      close() {
        const st = this[S];
        if (st.closeRequested || st.state !== 'readable') throw new TypeError('the stream is already closing');
        st.closeRequested = true;
        if (!st.queue.length) finish(st);
      }
      error(e) { if (this[S].state === 'readable') fail(this[S], e); }
    }
    const finish = (st) => {
      st.state = 'closed';
      for (const r of st.reads.splice(0)) r.resolve({ value: undefined, done: true });
      if (st.reader) st.reader[S].closed.resolve();
    };
    const fail = (st, e) => {
      st.state = 'errored'; st.error = e; st.queue = []; st.queueSize = 0;
      for (const r of st.reads.splice(0)) r.reject(e);
      if (st.reader) { const c = st.reader[S].closed; c.promise.catch(() => {}); c.reject(e); }
    };
    const pull = (st) => {
      if (!st.started || st.state !== 'readable' || st.closeRequested || !st.source.pull) return;
      if (st.hwm - st.queueSize <= 0 && !st.reads.length) return;
      if (st.pulling) { st.pullAgain = true; return; }
      st.pulling = true;
      Promise.resolve().then(() => st.source.pull.call(st.sourceObject, st.controller)).then(() => {
        st.pulling = false;
        if (st.pullAgain) { st.pullAgain = false; pull(st); }
      }, e => fail(st, e));
    };
    const deferred = () => { const d = {}; d.promise = new Promise((res, rej) => { d.resolve = res; d.reject = rej; }); return d; };
    class ReadableStreamDefaultReader {
      constructor(stream) {
        if (!(stream instanceof ReadableStream)) throw new TypeError('not a ReadableStream');
        const st = stream[S];
        if (st.reader) throw new TypeError('the stream is locked');
        st.reader = this;
        const closed = deferred();
        this[S] = { st, closed };
        if (st.state === 'closed') closed.resolve();
        else if (st.state === 'errored') { closed.promise.catch(() => {}); closed.reject(st.error); }
      }
      get closed() { return this[S].closed.promise; }
      read() {
        const st = this[S].st;
        if (!st) return Promise.reject(new TypeError('the reader was released'));
        if (st.state === 'errored') return Promise.reject(st.error);
        if (st.queue.length) {
          const [value, size] = st.queue.shift();
          st.queueSize -= size;
          if (st.closeRequested && !st.queue.length) finish(st); else pull(st);
          return Promise.resolve({ value, done: false });
        }
        if (st.state === 'closed') return Promise.resolve({ value: undefined, done: true });
        const d = deferred();
        st.reads.push(d);
        pull(st);
        return d.promise;
      }
      cancel(reason) { const st = this[S].st; return st ? cancel(st, reason) : Promise.reject(new TypeError('the reader was released')); }
      releaseLock() {
        const st = this[S].st;
        if (!st) return;
        for (const r of st.reads.splice(0)) r.reject(new TypeError('the reader was released'));
        if (st.state === 'readable') { const c = this[S].closed; c.promise.catch(() => {}); c.reject(new TypeError('the reader was released')); }
        st.reader = null;
        this[S].st = null;
      }
    }
    const cancel = (st, reason) => {
      if (st.state === 'closed') return Promise.resolve();
      if (st.state === 'errored') return Promise.reject(st.error);
      st.queue = []; st.queueSize = 0;
      finish(st);
      return Promise.resolve(st.source.cancel ? st.source.cancel.call(st.sourceObject, reason) : undefined).then(() => undefined);
    };
    class ReadableStream {
      constructor(source = {}, strategy = {}) {
        if (source === null) throw new TypeError('the source must be an object');
        if (source.type === 'bytes') { /* read as a default stream: chunks come as they're enqueued */ }
        const controller = Object.create(ReadableStreamDefaultController.prototype);
        const hwm = strategy.highWaterMark === undefined ? 1 : Number(strategy.highWaterMark);
        if (Number.isNaN(hwm) || hwm < 0) throw new RangeError('invalid highWaterMark');
        const st = { state: 'readable', queue: [], queueSize: 0, reads: [], reader: null, closeRequested: false, started: false,
                     pulling: false, pullAgain: false, hwm, size: strategy.size, controller, sourceObject: source,
                     source: { pull: source.pull, cancel: source.cancel } };
        controller[S] = st;
        this[S] = st;
        Promise.resolve(source.start ? source.start.call(source, controller) : undefined).then(() => { st.started = true; pull(st); }, e => fail(st, e));
      }
      get locked() { return !!this[S].reader; }
      getReader(options) {
        if (options && options.mode === 'byob') throw new TypeError('byob readers are not supported');
        return new ReadableStreamDefaultReader(this);
      }
      cancel(reason) { return this[S].reader ? Promise.reject(new TypeError('the stream is locked')) : cancel(this[S], reason); }
      tee() {
        const reader = this.getReader();
        let canceled = 0, reading = false;
        const branches = [];
        const pump = () => {
          if (reading) return Promise.resolve();
          reading = true;
          return reader.read().then(({ value, done }) => {
            reading = false;
            for (const b of branches) if (!b.canceled) { if (done) b.c.close(); else b.c.enqueue(value); }
          }, e => { for (const b of branches) b.c.error(e); });
        };
        for (let i = 0; i < 2; i++) {
          const b = { canceled: false, c: null };
          branches.push(b);
        }
        return branches.map(b => new ReadableStream({
          start(c) { b.c = c; },
          pull: pump,
          cancel: (r) => { b.canceled = true; if (++canceled === 2) return reader.cancel(r); },
        }));
      }
      pipeTo(dest, options = {}) {
        const reader = this.getReader();
        const writer = dest.getWriter();
        return new Promise((resolve, reject) => {
          const step = () => reader.read().then(({ value, done }) => {
            if (done) {
              reader.releaseLock(); writer.releaseLock();
              return (options.preventClose ? Promise.resolve() : dest.close()).then(resolve, reject);
            }
            return writer.write(value).then(step);
          }, e => { reader.releaseLock(); writer.releaseLock(); if (!options.preventAbort) dest.abort(e); reject(e); });
          step().catch(e => { reader.releaseLock(); writer.releaseLock(); if (!options.preventCancel) this.cancel(e); reject(e); });
        });
      }
      pipeThrough({ writable, readable }, options) { this.pipeTo(writable, options).catch(() => {}); return readable; }
      values({ preventCancel = false } = {}) {
        const reader = this.getReader();
        return {
          next: () => reader.read().then(r => { if (r.done) reader.releaseLock(); return r; }),
          return: (value) => { if (!preventCancel) reader.cancel(value); reader.releaseLock(); return Promise.resolve({ value, done: true }); },
          [Symbol.asyncIterator]() { return this; },
        };
      }
      [Symbol.asyncIterator](options) { return this.values(options); }
      static from(iterable) {
        const it = iterable[Symbol.asyncIterator] ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
        return new ReadableStream({
          pull(c) { return Promise.resolve(it.next()).then(r => (r.done ? c.close() : c.enqueue(r.value))); },
          cancel(r) { if (it.return) return it.return(r); },
        });
      }
    }
    class WritableStream {
      constructor(sink = {}) {
        const st = this[S] = { state: 'writable', writer: null, chain: Promise.resolve(), sink };
        const controller = { error: (e) => { st.state = 'errored'; st.error = e; }, signal: undefined };
        st.controller = controller;
        st.chain = Promise.resolve(sink.start ? sink.start.call(sink, controller) : undefined);
      }
      get locked() { return !!this[S].writer; }
      getWriter() {
        const st = this[S];
        if (st.writer) throw new TypeError('the stream is locked');
        const stream = this;
        const writer = {
          get desiredSize() { return st.state === 'writable' ? 1 : 0; },
          get ready() { return Promise.resolve(); },
          get closed() { return st.closedPromise || (st.closedPromise = new Promise((res, rej) => { st.onClosed = res; st.onErrored = rej; })); },
          write(chunk) {
            if (st.state !== 'writable') return Promise.reject(st.error || new TypeError('the stream is closed'));
            return (st.chain = st.chain.then(() => (st.sink.write ? st.sink.write.call(st.sink, chunk, st.controller) : undefined)).then(() => undefined));
          },
          close() { return stream.close(); },
          abort(r) { return stream.abort(r); },
          releaseLock() { st.writer = null; },
        };
        st.writer = writer;
        return writer;
      }
      close() {
        const st = this[S];
        if (st.state !== 'writable') return Promise.reject(new TypeError('the stream is closed'));
        st.state = 'closing';
        return (st.chain = st.chain.then(() => (st.sink.close ? st.sink.close.call(st.sink) : undefined)).then(() => { st.state = 'closed'; if (st.onClosed) st.onClosed(); }));
      }
      abort(reason) {
        const st = this[S];
        st.state = 'errored'; st.error = reason;
        if (st.onErrored) st.onErrored(reason);
        return Promise.resolve(st.sink.abort ? st.sink.abort.call(st.sink, reason) : undefined).then(() => undefined);
      }
    }
    class TransformStream {
      constructor(transformer = {}) {
        let rc;
        this.readable = new ReadableStream({ start(c) { rc = c; } });
        const controller = {
          enqueue: (chunk) => rc.enqueue(chunk), error: (e) => rc.error(e), terminate: () => rc.close(),
          get desiredSize() { return rc.desiredSize; },
        };
        if (transformer.start) transformer.start.call(transformer, controller);
        this.writable = new WritableStream({
          write: (chunk) => (transformer.transform ? transformer.transform.call(transformer, chunk, controller) : controller.enqueue(chunk)),
          close: () => Promise.resolve(transformer.flush ? transformer.flush.call(transformer, controller) : undefined).then(() => { try { rc.close(); } catch (e) {} }),
          abort: (r) => rc.error(r),
        });
      }
    }
    class CountQueuingStrategy { constructor({ highWaterMark }) { this.highWaterMark = highWaterMark; } size() { return 1; } }
    class ByteLengthQueuingStrategy { constructor({ highWaterMark }) { this.highWaterMark = highWaterMark; } size(c) { return c.byteLength; } }
    for (const C of [ReadableStream, ReadableStreamDefaultReader, ReadableStreamDefaultController, WritableStream, TransformStream,
                     CountQueuingStrategy, ByteLengthQueuingStrategy])
      Object.defineProperty(global, C.name, { value: C, writable: true, configurable: true });
  }

  // getSelection(): nothing is ever selected in the page (no text selection yet), but the
  // Selection is there to ask and to clear.
  if (typeof global.getSelection !== 'function') {
    class Selection {
      constructor(key) { if (key !== Selection) throw new TypeError('Illegal constructor'); this._ranges = []; }
      get rangeCount() { return this._ranges.length; }
      get isCollapsed() { return true; }
      get type() { return this._ranges.length ? 'Caret' : 'None'; }
      get anchorNode() { return this._ranges.length ? this._ranges[0].startContainer : null; }
      get anchorOffset() { return this._ranges.length ? this._ranges[0].startOffset : 0; }
      get focusNode() { return this.anchorNode; }
      get focusOffset() { return this.anchorOffset; }
      get direction() { return 'none'; }
      getRangeAt(i) {
        if (i < 0 || i >= this._ranges.length) throw new DOMException('no range at ' + i, 'IndexSizeError');
        return this._ranges[i];
      }
      addRange(r) { if (!this._ranges.length) this._ranges.push(r); }
      removeRange(r) { this._ranges = this._ranges.filter(x => x !== r); }
      removeAllRanges() { this._ranges = []; }
      empty() { this._ranges = []; }
      collapse(node, offset = 0) {
        this._ranges = [];
        if (node && typeof document.createRange === 'function') { const r = document.createRange(); try { r.setStart(node, offset); r.collapse(true); } catch (e) {} this._ranges.push(r); }
      }
      setPosition(node, offset) { this.collapse(node, offset); }
      collapseToStart() {} collapseToEnd() {} extend() {} selectAllChildren() {} deleteFromDocument() {} modify() {}
      setBaseAndExtent(node, offset) { this.collapse(node, offset); }
      containsNode() { return false; }
      toString() { return ''; }
    }
    const selection = new Selection(Selection);
    Object.defineProperty(global, 'Selection', { value: Selection, writable: true, configurable: true });
    Object.defineProperty(global, 'getSelection', { value: function getSelection() { return selection; }, writable: true, configurable: true });
    if (typeof Document === 'function' && !Document.prototype.getSelection)
      Object.defineProperty(Document.prototype, 'getSelection', { value: function getSelection() { return selection; }, writable: true, configurable: true });
  }

  // Text.splitText (Svelte's hydration splits text nodes: news.daum.net).
  if (typeof Text === 'function' && !Text.prototype.splitText)
    Object.defineProperty(Text.prototype, 'splitText', { configurable: true, writable: true, value: function splitText(offset) {
      offset = offset >>> 0;
      const data = this.data;
      if (offset > data.length) throw new DOMException('the offset is past the end of the text', 'IndexSizeError');
      const rest = (this.ownerDocument || document).createTextNode(data.slice(offset));
      this.data = data.slice(0, offset);
      if (this.parentNode) this.parentNode.insertBefore(rest, this.nextSibling);
      return rest;
    } });
  if (typeof Text === 'function')   // (the native one gave only its own text)
    Object.defineProperty(Text.prototype, 'wholeText', { configurable: true, enumerable: true, get() {
      let first = this, last = this, text = '';
      while (first.previousSibling && first.previousSibling.nodeType === 3) first = first.previousSibling;
      while (last.nextSibling && last.nextSibling.nodeType === 3) last = last.nextSibling;
      for (let n = first; ; n = n.nextSibling) { text += n.data; if (n === last) break; }
      return text;
    } });

  // MediaError, for a media element's error (Reddit's player checks against it).
  if (typeof global.MediaError !== 'function') {
    class MediaError {
      constructor() { throw new TypeError('Illegal constructor'); }
    }
    for (const [k, v] of [['MEDIA_ERR_ABORTED', 1], ['MEDIA_ERR_NETWORK', 2], ['MEDIA_ERR_DECODE', 3], ['MEDIA_ERR_SRC_NOT_SUPPORTED', 4]]) {
      Object.defineProperty(MediaError, k, { value: v, enumerable: true });
      Object.defineProperty(MediaError.prototype, k, { value: v, enumerable: true });
    }
    Object.defineProperty(global, 'MediaError', { value: MediaError, writable: true, configurable: true });
  }

  // navigator.javaEnabled(): there's no Java (analytics still ask: Danawa).
  if (typeof navigator === 'object' && typeof navigator.javaEnabled !== 'function')
    Object.defineProperty(navigator, 'javaEnabled', { value: function javaEnabled() { return false; }, writable: true, configurable: true });

  // navigator.serviceWorker: there, but no worker is ever registered or in control.
  if (typeof navigator === 'object' && !('serviceWorker' in navigator) && typeof EventTarget === 'function') {
    const sw = new (class ServiceWorkerContainer extends EventTarget {
      get controller() { return null; }
      get ready() { return new Promise(() => {}); }
      register() { return Promise.reject(new DOMException('service workers are not supported here', 'NotSupportedError')); }
      getRegistration() { return Promise.resolve(undefined); }
      getRegistrations() { return Promise.resolve([]); }
      startMessages() {}
    })();
    sw.oncontrollerchange = sw.onmessage = sw.onmessageerror = null;
    Object.defineProperty(navigator, 'serviceWorker', { value: sw, configurable: true, enumerable: true });
  }

  // .text: a script's, a link's and a title's text (Bing runs its type="text/rms" blocks
  // from it).
  for (const name of ['HTMLScriptElement', 'HTMLAnchorElement', 'HTMLTitleElement'])
    if (typeof global[name] === 'function' && !Object.getOwnPropertyDescriptor(global[name].prototype, 'text'))
      Object.defineProperty(global[name].prototype, 'text', {
        get() { return this.textContent; }, set(v) { this.textContent = String(v); }, configurable: true, enumerable: true,
      });

  // An <iframe>'s window, as a cross-origin one looks from outside: we don't run what's
  // inside, so messages to it go nowhere, but scripts that post to it carry on.
  if (typeof HTMLIFrameElement === 'function') {
    const frames = new WeakMap();
    Object.defineProperty(HTMLIFrameElement.prototype, 'contentWindow', {
      get() {
        if (!this.isConnected) return null;
        let w = frames.get(this);
        if (!w) {
          const frame = this;
          w = Object.freeze({
            postMessage() {}, focus() {}, blur() {}, close() {},
            addEventListener() {}, removeEventListener() {}, dispatchEvent() { return true; },
            get closed() { return !frame.isConnected; },
            get parent() { return global; }, get top() { return global; }, get opener() { return null; },
            get frames() { return w; }, get self() { return w; }, get window() { return w; }, length: 0,
            get location() { return { get href() { return frame.src || 'about:blank'; }, replace() {}, assign() {} }; },
          });
          frames.set(this, w);
        }
        return w;
      },
      configurable: true, enumerable: true,
    });
  }

  // Window: the global object's interface (window instanceof Window, Window.prototype).
  if (typeof global.Window !== 'function' && typeof EventTarget === 'function') {
    const proto = Object.create(EventTarget.prototype);
    function Window() { throw new TypeError('Illegal constructor'); }
    Object.defineProperty(Window, 'prototype', { value: proto, writable: false });
    Object.defineProperty(proto, 'constructor', { value: Window, writable: true, configurable: true });
    Object.defineProperty(proto, Symbol.toStringTag, { value: 'Window', configurable: true });
    try { if (Object.getPrototypeOf(global) === Object.prototype) Object.setPrototypeOf(global, proto); } catch (e) {}
    Object.defineProperty(global, 'Window', { value: Window, writable: true, configurable: true });
  }

  // Intl, in English (US) whatever locale is asked for: the formats pages show dates,
  // numbers and lists in, without the full locale data.
  const Intl = {};
  {
    const locale = 'en-US';
    const resolved = l => locale;
    const pad2 = n => String(n).padStart(2, '0');
    const groupDigits = s => s.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
    class NumberFormat {
      constructor(locales, o = {}) {
        this._o = { style: o.style || 'decimal', currency: (o.currency || 'USD').toUpperCase(), currencyDisplay: o.currencyDisplay || 'symbol',
          notation: o.notation || 'standard', compactDisplay: o.compactDisplay || 'short', useGrouping: o.useGrouping !== false,
          unit: o.unit, unitDisplay: o.unitDisplay || 'short', signDisplay: o.signDisplay || 'auto' };
        const cur = this._o.style === 'currency';
        this._min = o.minimumFractionDigits ?? (cur ? 2 : 0);
        this._max = Math.max(this._min, o.maximumFractionDigits ?? (cur ? 2 : this._o.style === 'percent' ? 0 : 3));
        if (o.maximumSignificantDigits) this._sig = o.maximumSignificantDigits;
      }
      format(n) {
        n = Number(n);
        if (Number.isNaN(n)) return 'NaN';
        const o = this._o;
        let suffix = '', prefix = '';
        if (o.style === 'percent') n *= 100;
        if (o.notation === 'compact') {
          for (const [v, s, l] of [[1e12, 'T', ' trillion'], [1e9, 'B', ' billion'], [1e6, 'M', ' million'], [1e3, 'K', ' thousand']]) {
            if (Math.abs(n) >= v) { n /= v; suffix = o.compactDisplay === 'long' ? l : s; break; }
          }
          if (!this._sig) { const r = Math.abs(n) < 100 ? Math.round(n * 10) / 10 : Math.round(n); n = r; }
        }
        const neg = n < 0 || Object.is(n, -0) && o.signDisplay === 'negative';
        n = Math.abs(n);
        let text;
        if (!Number.isFinite(n)) text = '∞';
        else if (this._sig) text = String(Number(n.toPrecision(this._sig)));
        else {
          text = n.toFixed(this._max);
          if (text.includes('.')) { text = text.replace(/0+$/, ''); const dot = text.indexOf('.'); const have = dot < 0 ? 0 : text.length - dot - 1; if (have < this._min) text = (dot < 0 ? text + '.' : text) + '0'.repeat(this._min - have); if (text.endsWith('.')) text = text.slice(0, -1); }
        }
        const [int, frac] = text.split('.');
        text = (o.useGrouping ? groupDigits(int) : int) + (frac ? '.' + frac : '');
        if (o.style === 'percent') suffix += '%';
        if (o.style === 'currency') {
          const symbols = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', KRW: '₩', CNY: 'CN¥', INR: '₹', CAD: 'CA$', AUD: 'A$' };
          prefix = o.currencyDisplay === 'code' ? o.currency + ' ' : symbols[o.currency] || o.currency + ' ';
        }
        if (o.style === 'unit' && o.unit) suffix += (o.unitDisplay === 'long' ? ' ' + o.unit + (n === 1 ? '' : 's') : ' ' + o.unit);
        const sign = neg ? '-' : (o.signDisplay === 'always' || (o.signDisplay === 'exceptZero' && n !== 0)) ? '+' : '';
        return sign + prefix + text + suffix;
      }
      formatToParts(n) { return [{ type: 'literal', value: this.format(n) }]; }
      resolvedOptions() { return { locale, numberingSystem: 'latn', ...this._o, minimumFractionDigits: this._min, maximumFractionDigits: this._max }; }
      static supportedLocalesOf(l) { return [].concat(l || []).length ? [locale] : []; }
    }
    const monthsLong = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
    const daysLong = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
    class DateTimeFormat {
      constructor(locales, o = {}) {
        this._o = { ...o };
        this._utc = o.timeZone && /^(utc|gmt|etc\/utc)$/i.test(o.timeZone);
        const any = ['weekday', 'era', 'year', 'month', 'day', 'hour', 'minute', 'second', 'dateStyle', 'timeStyle'].some(k => o[k] !== undefined);
        if (!any) Object.assign(this._o, { year: 'numeric', month: 'numeric', day: 'numeric' });
        if (o.dateStyle) Object.assign(this._o, { full: { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' }, long: { year: 'numeric', month: 'long', day: 'numeric' },
          medium: { year: 'numeric', month: 'short', day: 'numeric' }, short: { year: '2-digit', month: 'numeric', day: 'numeric' } }[o.dateStyle] || {});
        if (o.timeStyle) Object.assign(this._o, { hour: 'numeric', minute: '2-digit', ...(o.timeStyle === 'short' ? {} : { second: '2-digit' }) });
      }
      _fields(d) {
        d = d === undefined ? new Date() : new Date(typeof d === 'object' ? d.getTime() : d);
        if (Number.isNaN(d.getTime())) throw new RangeError('Invalid time value');
        const u = this._utc;
        return { y: u ? d.getUTCFullYear() : d.getFullYear(), m: u ? d.getUTCMonth() : d.getMonth(), d: u ? d.getUTCDate() : d.getDate(),
                 w: u ? d.getUTCDay() : d.getDay(), h: u ? d.getUTCHours() : d.getHours(), mi: u ? d.getUTCMinutes() : d.getMinutes(),
                 s: u ? d.getUTCSeconds() : d.getSeconds(), date: d };
      }
      formatToParts(date) {
        const f = this._fields(date), o = this._o, parts = [];
        const add = (type, value) => { if (parts.length) parts.push({ type: 'literal', value: type === 'dayPeriod' ? ' ' : parts[parts.length - 1].sep || ', ' }); parts.push({ type, value: String(value) }); };
        const textMonth = o.month === 'long' || o.month === 'short' || o.month === 'narrow';
        if (o.weekday) add('weekday', o.weekday === 'long' ? daysLong[f.w] : daysLong[f.w].slice(0, o.weekday === 'narrow' ? 1 : 3));
        if (textMonth) {
          const m = o.month === 'long' ? monthsLong[f.m] : monthsLong[f.m].slice(0, o.month === 'narrow' ? 1 : 3);
          if (parts.length) parts.push({ type: 'literal', value: ', ' });
          parts.push({ type: 'month', value: m });
          if (o.day) parts.push({ type: 'literal', value: ' ' }, { type: 'day', value: String(o.day === '2-digit' ? pad2(f.d) : f.d) });
          if (o.year) parts.push({ type: 'literal', value: o.day ? ', ' : ' ' }, { type: 'year', value: String(o.year === '2-digit' ? pad2(f.y % 100) : f.y) });
        } else if (o.month || o.day || o.year) {
          const bits = [];
          if (o.month) bits.push(['month', o.month === '2-digit' ? pad2(f.m + 1) : f.m + 1]);
          if (o.day) bits.push(['day', o.day === '2-digit' ? pad2(f.d) : f.d]);
          if (o.year) bits.push(['year', o.year === '2-digit' ? pad2(f.y % 100) : f.y]);
          if (parts.length) parts.push({ type: 'literal', value: ', ' });
          bits.forEach(([t, v], i) => { if (i) parts.push({ type: 'literal', value: '/' }); parts.push({ type: t, value: String(v) }); });
        }
        if (o.hour || o.minute || o.second) {
          const h12 = o.hour12 !== false && o.hourCycle !== 'h23';
          if (parts.length) parts.push({ type: 'literal', value: ', ' });
          const hv = h12 ? (f.h % 12 || 12) : f.h;
          const tbits = [];
          if (o.hour) tbits.push(['hour', o.hour === '2-digit' || !h12 ? pad2(hv) : hv]);
          if (o.minute) tbits.push(['minute', pad2(f.mi)]);
          if (o.second) tbits.push(['second', pad2(f.s)]);
          tbits.forEach(([t, v], i) => { if (i) parts.push({ type: 'literal', value: ':' }); parts.push({ type: t, value: String(v) }); });
          if (h12 && o.hour) parts.push({ type: 'literal', value: ' ' }, { type: 'dayPeriod', value: f.h < 12 ? 'AM' : 'PM' });
        }
        if (o.timeZoneName) parts.push({ type: 'literal', value: ' ' }, { type: 'timeZoneName', value: this._utc ? 'UTC' : (String(f.date).match(/\(([^)]+)\)/) || [, 'GMT'])[1] });
        return parts;
      }
      format(date) { return this.formatToParts(date).map(p => p.value).join(''); }
      formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
      resolvedOptions() { return { locale, calendar: 'gregory', numberingSystem: 'latn', timeZone: this._o.timeZone || (this._utc ? 'UTC' : 'UTC'), ...this._o }; }
      static supportedLocalesOf(l) { return [].concat(l || []).length ? [locale] : []; }
    }
    class RelativeTimeFormat {
      constructor(locales, o = {}) { this._numeric = o.numeric || 'always'; this._style = o.style || 'long'; }
      format(v, unit) {
        v = Number(v); unit = String(unit).replace(/s$/, '');
        if (this._numeric === 'auto') {
          const words = { day: { '-1': 'yesterday', '0': 'today', '1': 'tomorrow' }, year: { '-1': 'last year', '0': 'this year', '1': 'next year' },
                          month: { '-1': 'last month', '0': 'this month', '1': 'next month' }, week: { '-1': 'last week', '0': 'this week', '1': 'next week' },
                          second: { '0': 'now' }, minute: { '0': 'this minute' }, hour: { '0': 'this hour' } };
          if (words[unit] && words[unit][String(v)] !== undefined) return words[unit][String(v)];
        }
        const n = Math.abs(v), label = n === 1 ? unit : unit + 's';
        const shown = new NumberFormat().format(n) + ' ' + (this._style === 'long' ? label : { second: 'sec.', minute: 'min.', hour: 'hr.', day: n === 1 ? 'day' : 'days', week: 'wk.', month: 'mo.', year: 'yr.', quarter: 'qtr.' }[unit] || label);
        return v < 0 || Object.is(v, -0) ? shown + ' ago' : 'in ' + shown;
      }
      formatToParts(v, unit) { return [{ type: 'literal', value: this.format(v, unit) }]; }
      resolvedOptions() { return { locale, style: this._style, numeric: this._numeric, numberingSystem: 'latn' }; }
    }
    class PluralRules {
      constructor(locales, o = {}) { this._type = o.type || 'cardinal'; }
      select(n) {
        n = Number(n);
        if (this._type === 'ordinal') { const t = n % 10, h = n % 100; return t === 1 && h !== 11 ? 'one' : t === 2 && h !== 12 ? 'two' : t === 3 && h !== 13 ? 'few' : 'other'; }
        return n === 1 ? 'one' : 'other';
      }
      resolvedOptions() { return { locale, type: this._type, pluralCategories: this._type === 'ordinal' ? ['few', 'one', 'two', 'other'] : ['one', 'other'] }; }
    }
    class ListFormat {
      constructor(locales, o = {}) { this._type = o.type || 'conjunction'; this._style = o.style || 'long'; }
      format(list) {
        const a = Array.from(list, String), word = this._type === 'disjunction' ? 'or' : 'and';
        if (this._type === 'unit') return a.join(this._style === 'narrow' ? ' ' : ', ');
        if (a.length < 3) return a.join(' ' + word + ' ');
        return a.slice(0, -1).join(', ') + ', ' + word + ' ' + a[a.length - 1];
      }
      formatToParts(list) { return [{ type: 'literal', value: this.format(list) }]; }
    }
    class Collator {
      constructor(locales, o = {}) { this._sens = o.sensitivity || 'variant'; this._numeric = !!o.numeric; }
      compare(a, b) {
        a = String(a); b = String(b);
        if (this._sens === 'base' || this._sens === 'accent') { a = a.toLowerCase(); b = b.toLowerCase(); }
        if (this._numeric) {
          const re = /(\d+|\D+)/g, x = a.match(re) || [], y = b.match(re) || [];
          for (let i = 0; i < Math.min(x.length, y.length); i++) {
            if (x[i] === y[i]) continue;
            const nx = Number(x[i]), ny = Number(y[i]);
            if (!Number.isNaN(nx) && !Number.isNaN(ny)) return nx < ny ? -1 : 1;
            return x[i].toLowerCase() < y[i].toLowerCase() ? -1 : x[i].toLowerCase() > y[i].toLowerCase() ? 1 : x[i] < y[i] ? 1 : -1;
          }
          return x.length - y.length;
        }
        const la = a.toLowerCase(), lb = b.toLowerCase();
        if (la !== lb) return la < lb ? -1 : 1;
        return a === b ? 0 : a < b ? 1 : -1;   // lower case first, as ICU's root collation
      }
      resolvedOptions() { return { locale, usage: 'sort', sensitivity: this._sens, numeric: this._numeric }; }
    }
    class Segmenter {
      constructor(locales, o = {}) { this._g = o.granularity || 'grapheme'; }
      segment(text) {
        text = String(text);
        const out = [];
        if (this._g === 'word') { const re = /[\p{L}\p{N}_']+|[^\p{L}\p{N}_']+/gu; let m; while ((m = re.exec(text))) out.push({ segment: m[0], index: m.index, input: text, isWordLike: /[\p{L}\p{N}]/u.test(m[0]) }); }
        else if (this._g === 'sentence') { const re = /[^.!?]+[.!?]*\s*/g; let m; while ((m = re.exec(text))) out.push({ segment: m[0], index: m.index, input: text }); }
        else { let i = 0; for (const ch of text) { out.push({ segment: ch, index: i, input: text }); i += ch.length; } }
        out.containing = idx => out.find(s => idx >= s.index && idx < s.index + s.segment.length);
        return out;
      }
    }
    class DisplayNames {
      constructor(locales, o = {}) { this._type = o.type; }
      of(code) {
        const names = { language: { en: 'English', ko: 'Korean', ja: 'Japanese', zh: 'Chinese', de: 'German', fr: 'French', es: 'Spanish' },
                        region: { US: 'United States', KR: 'South Korea', JP: 'Japan', GB: 'United Kingdom', DE: 'Germany', FR: 'France' },
                        currency: { USD: 'US Dollar', EUR: 'Euro', KRW: 'South Korean Won', JPY: 'Japanese Yen' } };
        return (names[this._type] || {})[code] || code;
      }
    }
    // compare and format are getters giving bound functions: they work as callbacks.
    for (const [C, name] of [[Collator, 'compare'], [NumberFormat, 'format'], [DateTimeFormat, 'format']]) {
      const plain = C.prototype[name];
      const cache = Symbol(name);
      Object.defineProperty(C.prototype, name, { configurable: true, get() { return this[cache] || (this[cache] = plain.bind(this)); } });
    }
    // These three may be called without new (Intl.DateTimeFormat().resolvedOptions()).
    const callable = (C) => new Proxy(C, { apply(target, self, args) { return new target(...args); } });
    Object.assign(Intl, { NumberFormat: callable(NumberFormat), DateTimeFormat: callable(DateTimeFormat), RelativeTimeFormat, PluralRules, ListFormat,
                          Collator: callable(Collator), Segmenter, DisplayNames,
      getCanonicalLocales: l => [].concat(l || []).map(String), supportedValuesOf: () => [] });
    Object.defineProperty(Intl, Symbol.toStringTag, { value: 'Intl', configurable: true });
    // The library's locale-sensitive methods, through the same formats.
    Number.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this.valueOf()); };
    Date.prototype.toLocaleDateString = function (l, o = {}) { return new DateTimeFormat(l, Object.keys(o).length ? o : { year: 'numeric', month: 'numeric', day: 'numeric' }).format(this); };
    Date.prototype.toLocaleTimeString = function (l, o = {}) { return new DateTimeFormat(l, Object.keys(o).length ? o : { hour: 'numeric', minute: '2-digit', second: '2-digit' }).format(this); };
    Date.prototype.toLocaleString = function (l, o = {}) { return new DateTimeFormat(l, Object.keys(o).length ? o : { year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: '2-digit', second: '2-digit' }).format(this); };
    String.prototype.localeCompare = function (that, l, o) { return new Collator(l, o).compare(String(this), that); };
  }

  // A link's address in parts, as HTMLAnchorElement and HTMLAreaElement have them.
  for (const name of ['HTMLAnchorElement', 'HTMLAreaElement']) {
    const proto = global[name] && global[name].prototype;
    if (!proto) continue;
    const url = el => { try { return new URL(el.getAttribute('href') || '', document.baseURI); } catch (e) { return null; } };
    for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password']) {
      Object.defineProperty(proto, part, {
        get() { const u = url(this); return u ? u[part] : ''; },
        set(v) { const u = url(this); if (u && part !== 'origin') { u[part] = v; this.setAttribute('href', u.href); } },
        configurable: true, enumerable: true });
    }
  }

  // insertAdjacentElement and insertAdjacentText, beside insertAdjacentHTML.
  {
    const place = (el, where, node) => {
      switch (String(where).toLowerCase()) {
        case 'beforebegin': if (!el.parentNode) return null; el.parentNode.insertBefore(node, el); return node;
        case 'afterbegin': el.insertBefore(node, el.firstChild); return node;
        case 'beforeend': el.appendChild(node); return node;
        case 'afterend': if (!el.parentNode) return null; el.parentNode.insertBefore(node, el.nextSibling); return node;
        default: throw new SyntaxError("insertAdjacent: '" + where + "' isn't a position");
      }
    };
    Element.prototype.insertAdjacentElement = function (where, el) { return place(this, where, el); };
    Element.prototype.insertAdjacentText = function (where, text) { place(this, where, document.createTextNode(String(text))); };
  }

  // DOMParser and XMLSerializer.
  {
    const impl = document.implementation;
    const parse = impl.__parseDocument;
    delete impl.__parseDocument;
    class DOMParser {
      parseFromString(text, type) {
        const t = String(type);
        if (!['text/html', 'text/xml', 'application/xml', 'application/xhtml+xml', 'image/svg+xml'].includes(t))
          throw new TypeError("DOMParser can't parse " + t);
        return parse.call(impl, String(text));
      }
    }
    class XMLSerializer {
      serializeToString(node) {
        if (node && node.nodeType === 9) return (node.documentElement ? node.documentElement.outerHTML : '');
        if (node && node.nodeType === 1) return node.outerHTML;
        return node && node.textContent != null ? String(node.textContent) : '';
      }
    }
    global.DOMParser = DOMParser;
    global.XMLSerializer = XMLSerializer;
  }

  // Range: positions in the tree, and the HTML-to-nodes use pages mostly want of it.
  class Range {
    constructor() {
      this.startContainer = document; this.startOffset = 0;
      this.endContainer = document; this.endOffset = 0;
    }
    get collapsed() { return this.startContainer === this.endContainer && this.startOffset === this.endOffset; }
    get commonAncestorContainer() {
      for (let a = this.startContainer; a; a = a.parentNode) if (a === this.endContainer || a.contains(this.endContainer)) return a;
      return document;
    }
    setStart(node, offset) { this.startContainer = node; this.startOffset = offset >>> 0; }
    setEnd(node, offset) { this.endContainer = node; this.endOffset = offset >>> 0; }
    setStartBefore(node) { this.setStart(node.parentNode, Array.prototype.indexOf.call(node.parentNode.childNodes, node)); }
    setStartAfter(node) { this.setStart(node.parentNode, Array.prototype.indexOf.call(node.parentNode.childNodes, node) + 1); }
    setEndBefore(node) { this.setEnd(node.parentNode, Array.prototype.indexOf.call(node.parentNode.childNodes, node)); }
    setEndAfter(node) { this.setEnd(node.parentNode, Array.prototype.indexOf.call(node.parentNode.childNodes, node) + 1); }
    selectNode(node) { this.setStartBefore(node); this.setEndAfter(node); }
    selectNodeContents(node) { this.setStart(node, 0); this.setEnd(node, node.childNodes ? node.childNodes.length : 0); }
    collapse(toStart) { if (toStart) this.setEnd(this.startContainer, this.startOffset); else this.setStart(this.endContainer, this.endOffset); }
    cloneRange() { const r = new Range(); r.setStart(this.startContainer, this.startOffset); r.setEnd(this.endContainer, this.endOffset); return r; }
    createContextualFragment(html) {
      const t = document.createElement('template');
      t.innerHTML = String(html);
      const f = document.createDocumentFragment();
      while (t.content.firstChild) f.appendChild(t.content.firstChild);
      return f;
    }
    getBoundingClientRect() {
      const n = this.startContainer;
      return n && n.getBoundingClientRect ? n.getBoundingClientRect() : { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 };
    }
    getClientRects() { return []; }
    deleteContents() {}
    detach() {}
    toString() { return ''; }
  }
  global.Range = Range;
  if (typeof Document === 'function') Document.prototype.createRange = function createRange() { return new Range(); };
  if (typeof Element === 'function' && !Element.prototype.webkitMatchesSelector)
    Element.prototype.webkitMatchesSelector = Element.prototype.matches;

  // Web Animations, without the animating: an animation finishes at once (as if it
  // had zero duration), keeping its last frame when its fill says so.
  if (typeof Element === 'function' && !Element.prototype.animate) {
    class Animation extends EventTarget {
      constructor(effect) {
        super();
        this.effect = effect; this.id = ''; this.playbackRate = 1; this.startTime = null; this.currentTime = null;
        this.playState = 'finished'; this.pending = false; this.onfinish = null; this.oncancel = null; this.onremove = null;
        this.finished = Promise.resolve(this);
        this.ready = Promise.resolve(this);
      }
      play() {} pause() {} finish() {} reverse() {} persist() {} commitStyles() {} updatePlaybackRate(r) { this.playbackRate = r; }
      cancel() { this.playState = 'idle'; }
    }
    const lastFrame = (frames) => {
      if (Array.isArray(frames)) return frames.length ? frames[frames.length - 1] : {};
      const out = {};
      if (frames && typeof frames === 'object')
        for (const k of Object.keys(frames)) { const v = frames[k]; out[k] = Array.isArray(v) ? v[v.length - 1] : v; }
      return out;
    };
    Element.prototype.animate = function animate(frames, options) {
      const fill = options && typeof options === 'object' ? options.fill : undefined;
      if ((fill === 'forwards' || fill === 'both') && this.style) {
        const last = lastFrame(frames);
        for (const k of Object.keys(last))
          if (k !== 'offset' && k !== 'easing' && k !== 'composite') try { this.style[k] = last[k]; } catch (e) {}
      }
      const a = new Animation({ target: this, getKeyframes: () => [], getTiming: () => (typeof options === 'object' ? options : { duration: options }) });
      setTimeout(() => {
        const e = new Event('finish');
        a.dispatchEvent(e);
        if (typeof a.onfinish === 'function') a.onfinish.call(a, e);
      }, 0);
      return a;
    };
    Element.prototype.getAnimations = function getAnimations() { return []; };
    if (typeof Document === 'function') Document.prototype.getAnimations = function getAnimations() { return []; };
    global.Animation = Animation;
  }

  // Constraint validation: validity, setCustomValidity, checkValidity, for the form
  // controls, and ElementInternals for form-associated custom elements.
  if (typeof Element === 'function' && typeof HTMLInputElement === 'function' && !HTMLInputElement.prototype.setCustomValidity) {
    const custom = new WeakMap();
    const flagNames = ['valueMissing', 'typeMismatch', 'patternMismatch', 'tooLong', 'tooShort', 'rangeUnderflow',
                       'rangeOverflow', 'stepMismatch', 'badInput', 'customError'];
    class ValidityState {
      constructor(flags) { for (const n of flagNames) Object.defineProperty(this, n, { value: !!flags[n], enumerable: true }); }
      get valid() { return !flagNames.some(n => this[n]); }
    }
    global.ValidityState = ValidityState;
    const barred = (el) => el.disabled || el.hasAttribute('readonly') && el.localName !== 'select' ||
      (el.localName === 'input' && ['hidden', 'reset', 'button'].includes(el.type)) ||
      el.localName === 'button' && el.type !== 'submit' || el.localName === 'fieldset' || el.localName === 'output' ||
      !!el.closest('datalist');
    const flagsOf = (el) => {
      const f = { customError: !!custom.get(el) };
      if (barred(el)) return f;
      const v = el.localName === 'select' || el.localName === 'textarea' || el.localName === 'input' ? String(el.value ?? '') : '';
      const type = el.localName === 'input' ? el.type : '';
      if (el.hasAttribute('required')) {
        if (type === 'checkbox' || type === 'radio') {
          if (type === 'checkbox') f.valueMissing = !el.checked;
          else {
            const name = el.name, scope = el.form || el.getRootNode();
            f.valueMissing = !(name ? Array.from(scope.querySelectorAll('input[type=radio]')).some(r => r.name === name && r.checked) : el.checked);
          }
        } else if (el.localName !== 'button') {
          f.valueMissing = v === '';
        }
      }
      if (v !== '') {
        if (type === 'email') f.typeMismatch = !v.split(el.multiple ? ',' : '\u0000').every(x => /^[^@\s]+@[^@\s]+$/.test(x.trim()));
        if (type === 'url') { try { new URL(v); } catch (e) { f.typeMismatch = true; } }
        const pattern = el.getAttribute('pattern');
        if (pattern !== null && ['text', 'search', 'url', 'tel', 'email', 'password'].includes(type)) {
          try { f.patternMismatch = !new RegExp('^(?:' + pattern + ')$', 'v').test(v); }
          catch (e) { try { f.patternMismatch = !new RegExp('^(?:' + pattern + ')$', 'u').test(v); } catch (e2) {} }
        }
        const max = el.getAttribute('maxlength'), min = el.getAttribute('minlength');
        if (max !== null && /^\d+$/.test(max) && v.length > +max) f.tooLong = true;
        if (min !== null && /^\d+$/.test(min) && v.length < +min) f.tooShort = true;
        if (type === 'number' || type === 'range') {
          const n = Number(v), lo = el.getAttribute('min'), hi = el.getAttribute('max');
          if (Number.isNaN(n)) f.badInput = true;
          else {
            if (lo !== null && lo !== '' && n < Number(lo)) f.rangeUnderflow = true;
            if (hi !== null && hi !== '' && n > Number(hi)) f.rangeOverflow = true;
          }
        }
      }
      return f;
    };
    const message = (el, f) => {
      if (f.customError) return custom.get(el);
      if (f.valueMissing) return 'Please fill out this field.';
      if (f.typeMismatch) return el.type === 'email' ? 'Please enter an email address.' : 'Please enter a URL.';
      if (f.patternMismatch) return 'Please match the requested format.';
      if (f.tooLong || f.tooShort) return 'Please use the requested length.';
      if (f.rangeUnderflow || f.rangeOverflow) return 'Please enter a value in range.';
      if (f.badInput) return 'Please enter a number.';
      return '';
    };
    const check = (el) => {
      const ok = new ValidityState(flagsOf(el)).valid;
      if (!ok) el.dispatchEvent(new Event('invalid', { cancelable: true }));
      return ok;
    };
    const controls = ['HTMLInputElement', 'HTMLSelectElement', 'HTMLTextAreaElement', 'HTMLButtonElement',
                      'HTMLFieldSetElement', 'HTMLOutputElement', 'HTMLObjectElement'];
    for (const name of controls) {
      const C = global[name];
      if (typeof C !== 'function') continue;
      const P = C.prototype;
      P.setCustomValidity = function setCustomValidity(msg) { custom.set(this, String(msg)); };
      Object.defineProperty(P, 'validity', { get() { return new ValidityState(flagsOf(this)); }, configurable: true, enumerable: true });
      Object.defineProperty(P, 'validationMessage', { get() { return barred(this) ? '' : message(this, flagsOf(this)); }, configurable: true, enumerable: true });
      Object.defineProperty(P, 'willValidate', { get() { return !barred(this); }, configurable: true, enumerable: true });
      P.checkValidity = function checkValidity() { return check(this); };
      P.reportValidity = function reportValidity() { return check(this); };
    }
    if (typeof HTMLFormElement === 'function') {
      const all = (form) => {
        let ok = true;
        for (const el of Array.from(form.elements)) if (typeof el.checkValidity === 'function' && !el.checkValidity()) ok = false;
        return ok;
      };
      HTMLFormElement.prototype.checkValidity = function checkValidity() { return all(this); };
      HTMLFormElement.prototype.reportValidity = function reportValidity() { return all(this); };
    }
    // ElementInternals: what a form-associated custom element tells its form.
    class ElementInternals {
      #el; #flags = {}; #message = ''; #value = null;
      constructor(el) { this.#el = el; this.states = new Set(); }
      get shadowRoot() { return this.#el.shadowRoot; }
      get form() { return this.#el.closest('form'); }
      get labels() { return this.#el.id ? Array.from(document.querySelectorAll('label[for="' + CSS.escape(this.#el.id) + '"]')) : []; }
      get willValidate() { return true; }
      get validity() { return new ValidityState(this.#flags); }
      get validationMessage() { return this.#message; }
      setFormValue(value) { this.#value = value; }
      setValidity(flags, message) {
        this.#flags = Object.assign({}, flags || {});
        this.#message = message === undefined ? '' : String(message);
      }
      checkValidity() {
        const ok = new ValidityState(this.#flags).valid;
        if (!ok) this.#el.dispatchEvent(new Event('invalid', { cancelable: true }));
        return ok;
      }
      reportValidity() { return this.checkValidity(); }
    }
    global.ElementInternals = ElementInternals;
    if (typeof HTMLElement === 'function' && !HTMLElement.prototype.attachInternals) {
      const attached = new WeakSet();
      HTMLElement.prototype.attachInternals = function attachInternals() {
        if (attached.has(this)) throw new DOMException('ElementInternals already attached', 'NotSupportedError');
        attached.add(this);
        return new ElementInternals(this);
      };
    }
  }

  // DOMTokenList (classList): iterable, as a list of its tokens.
  {
    const proto = Object.getPrototypeOf(document.createElement('div').classList);
    const tokens = list => Array.prototype.slice.call(list, 0, list.length);
    Object.defineProperty(proto, Symbol.iterator, { value() { return tokens(this)[Symbol.iterator](); }, writable: true, configurable: true });
    proto.values = function () { return tokens(this).values(); };
    proto.keys = function () { return tokens(this).keys(); };
    proto.entries = function () { return tokens(this).entries(); };
    proto.forEach = function (cb, thisArg) { tokens(this).forEach((v, i) => cb.call(thisArg, v, i, this)); };
  }

  // <select> and <option>, over the element's attributes and selectedIndex (which goes
  // through the page's form state).
  if (typeof HTMLSelectElement === 'function' && typeof HTMLOptionElement === 'function') {
    const def = (proto, name, get, set) => Object.defineProperty(proto, name, { get, set, configurable: true, enumerable: true });
    const S = HTMLSelectElement.prototype, O = HTMLOptionElement.prototype;
    const ownerSelect = o => {
      let p = o.parentNode;
      if (p && p.localName === 'optgroup') p = p.parentNode;
      return p && p.localName === 'select' ? p : null;
    };
    const collapse = t => String(t).replace(/[ \t\n\f\r]+/g, ' ').trim();
    def(O, 'text', function () { return collapse(this.textContent); }, function (v) { this.textContent = String(v); });
    def(O, 'label', function () { return this.hasAttribute('label') ? this.getAttribute('label') : this.text; },
        function (v) { this.setAttribute('label', String(v)); });
    def(O, 'value', function () { return this.hasAttribute('value') ? this.getAttribute('value') : this.text; },
        function (v) { this.setAttribute('value', String(v)); });
    def(O, 'defaultSelected', function () { return this.hasAttribute('selected'); },
        function (v) { if (v) this.setAttribute('selected', ''); else this.removeAttribute('selected'); });
    def(O, 'index', function () { const s = ownerSelect(this); return s ? Array.prototype.indexOf.call(s.options, this) : 0; });
    def(O, 'form', function () { const s = ownerSelect(this); return s ? s.form : null; });
    def(O, 'selected', function () {
      const s = ownerSelect(this);
      return s ? s.selectedIndex === this.index : this.hasAttribute('selected');
    }, function (v) {
      const s = ownerSelect(this);
      if (!s) { this.defaultSelected = v; return; }
      if (v) s.selectedIndex = this.index;
      else if (s.selectedIndex === this.index) s.selectedIndex = -1;
    });
    def(S, 'length', function () { return this.options.length; });
    def(S, 'selectedOptions', function () { return Array.prototype.filter.call(this.options, o => o.selected); });
    S.item = function (i) { return this.options[i >>> 0] || null; };
    S.namedItem = function (name) {
      return Array.prototype.find.call(this.options, o => o.id === name || o.getAttribute('name') === name) || null;
    };
    S.add = function (element, before) {
      let ref = null;
      if (typeof before === 'number') ref = this.options[before] || null;
      else if (before != null) ref = before;
      if (ref) ref.parentNode.insertBefore(element, ref);
      else this.appendChild(element);
    };
    const removeNode = Element.prototype.remove;
    S.remove = function (index) {
      if (arguments.length === 0) return removeNode.call(this);
      const o = this.options[index];
      if (o) o.remove();
    };
    global.Option = function Option(text, value, defaultSelected, selected) {
      if (!new.target) throw new TypeError("Constructor Option requires 'new'");
      const o = document.createElement('option');
      if (text !== undefined && text !== '') o.text = text;
      if (value !== undefined) o.value = value;
      if (defaultSelected) o.defaultSelected = true;
      if (selected) o.setAttribute('selected', '');
      return o;
    };
    global.Option.prototype = O;
  }
  if (typeof HTMLImageElement === 'function') {
    global.Image = function Image(width, height) {
      if (!new.target) throw new TypeError("Constructor Image requires 'new'");
      const img = document.createElement('img');
      if (width !== undefined) img.setAttribute('width', String(width >>> 0));
      if (height !== undefined) img.setAttribute('height', String(height >>> 0));
      return img;
    };
    global.Image.prototype = HTMLImageElement.prototype;
  }
  if (typeof HTMLAudioElement === 'function' && !global.Audio) {
    global.Audio = function Audio(src) {
      if (!new.target) throw new TypeError("Constructor Audio requires 'new'");
      const a = document.createElement('audio');
      a.setAttribute('preload', 'auto');
      if (src !== undefined) a.setAttribute('src', String(src));
      return a;
    };
    global.Audio.prototype = HTMLAudioElement.prototype;
  }

  // MessageChannel: two ports; what one posts arrives at the other as a task (a
  // "message" event), once it has started (onmessage starts it).
  class MessagePort extends EventTarget {
    constructor(key) {
      super();
      if (key !== portKey) throw new TypeError('Illegal constructor');
      this._other = null; this._started = false; this._closed = false; this._queue = []; this._onmessage = null; this.onmessageerror = null;
    }
    get onmessage() { return this._onmessage; }
    set onmessage(f) { this._onmessage = typeof f === 'function' ? f : null; this.start(); }
    postMessage(data) {
      const to = this._other;
      if (this._closed || !to || to._closed) return;
      const copy = structuredClone(data);
      to._queue.push(copy);
      if (to._started) to._flush();
    }
    start() { if (this._started) return; this._started = true; this._flush(); }
    close() { this._closed = true; this._queue.length = 0; }
    _flush() {
      if (this._pending) return;
      this._pending = true;
      setTimeout(() => {
        this._pending = false;
        while (this._queue.length && !this._closed) {
          const data = this._queue.shift();
          const e = new MessageEvent('message', { data, origin: '', ports: [] });
          this.dispatchEvent(e);
          if (this._onmessage) try { this._onmessage.call(this, e); } catch (x) { reportError(x); }
        }
      }, 0);
    }
  }
  const portKey = {};
  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort(portKey);
      this.port2 = new MessagePort(portKey);
      this.port1._other = this.port2;
      this.port2._other = this.port1;
    }
  }
  // BroadcastChannel: to the other channels of that name in this page.
  const channels = new Map();
  class BroadcastChannel extends EventTarget {
    constructor(name) {
      super();
      this.name = String(name); this.onmessage = null; this.onmessageerror = null; this._closed = false;
      if (!channels.has(this.name)) channels.set(this.name, new Set());
      channels.get(this.name).add(this);
    }
    postMessage(data) {
      if (this._closed) throw new DOMException('the channel is closed', 'InvalidStateError');
      for (const other of channels.get(this.name) || []) {
        if (other === this) continue;
        const copy = structuredClone(data);
        setTimeout(() => {
          if (other._closed) return;
          const e = new MessageEvent('message', { data: copy, origin: '' });
          other.dispatchEvent(e);   // (its onmessage too)
        }, 0);
      }
    }
    close() { this._closed = true; channels.get(this.name)?.delete(this); }
  }
  const reportError = (x) => setTimeout(() => { throw x; }, 0);
  // 2D canvas: CanvasRenderingContext2D, Path2D, CanvasGradient and ImageData. State,
  // transforms and paths live here; shapes go to the native side as polygons in canvas
  // pixels (__xwebCanvas2d), which keeps each canvas's bitmap.
  if (global.__xwebCanvas2d && typeof HTMLCanvasElement === 'function') {
    const N = global.__xwebCanvas2d;
    delete global.__xwebCanvas2d;
    const mul = (m, n) => [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
                           m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
    const scaleOf = (m) => Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1;
    const finite = (...xs) => xs.every(Number.isFinite);

    // Subpaths in canvas pixels, built through a transform.
    class Builder {
      constructor() { this.subs = []; this.cur = null; this.last = null; this.start = null; }
      _pt(m, x, y) { return [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]]; }
      moveTo(m, x, y) {
        const p = this._pt(m, x, y);
        this.cur = { pts: [p[0], p[1]], closed: false };
        this.subs.push(this.cur);
        this.last = [x, y]; this.start = [x, y];
      }
      ensure(m, x, y) { if (!this.cur) this.moveTo(m, x, y); }
      lineTo(m, x, y) {
        if (!this.cur) { this.moveTo(m, x, y); return; }
        const p = this._pt(m, x, y);
        this.cur.pts.push(p[0], p[1]);
        this.last = [x, y];
      }
      closePath() {
        if (!this.cur) return;
        this.cur.closed = true;
        if (this.start) { const s = this.start; this.cur = null; this.lastClosed = s; this.last = s; }
      }
      _resume(m) { if (!this.cur && this.last) this.moveTo(m, this.last[0], this.last[1]); }
      _steps(m, len) { return Math.max(4, Math.min(256, Math.ceil(len * scaleOf(m) / 3))); }
      quadraticCurveTo(m, cx, cy, x, y) {
        this._resume(m);
        if (!this.last) { this.moveTo(m, cx, cy); }
        const [x0, y0] = this.last;
        const n = this._steps(m, Math.hypot(cx - x0, cy - y0) + Math.hypot(x - cx, y - cy));
        for (let i = 1; i <= n; i++) {
          const t = i / n, u = 1 - t;
          this.lineTo(m, u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
        }
      }
      bezierCurveTo(m, c1x, c1y, c2x, c2y, x, y) {
        this._resume(m);
        if (!this.last) { this.moveTo(m, c1x, c1y); }
        const [x0, y0] = this.last;
        const n = this._steps(m, Math.hypot(c1x - x0, c1y - y0) + Math.hypot(c2x - c1x, c2y - c1y) + Math.hypot(x - c2x, y - c2y));
        for (let i = 1; i <= n; i++) {
          const t = i / n, u = 1 - t;
          this.lineTo(m, u * u * u * x0 + 3 * u * u * t * c1x + 3 * u * t * t * c2x + t * t * t * x,
                         u * u * u * y0 + 3 * u * u * t * c1y + 3 * u * t * t * c2y + t * t * t * y);
        }
      }
      ellipse(m, cx, cy, rx, ry, rot, a0, a1, ccw) {
        if (rx < 0 || ry < 0) throw new DOMException('the radius is negative', 'IndexSizeError');
        const tau = Math.PI * 2;
        let sweep = a1 - a0;
        if (!ccw) { if (sweep >= tau) sweep = tau; else { sweep %= tau; if (sweep < 0) sweep += tau; if (sweep === 0 && a1 !== a0) sweep = tau; } }
        else { if (-sweep >= tau) sweep = -tau; else { sweep %= tau; if (sweep > 0) sweep -= tau; if (sweep === 0 && a1 !== a0) sweep = -tau; } }
        const n = Math.max(2, Math.min(512, Math.ceil(Math.abs(sweep) * Math.max(rx, ry) * scaleOf(m) / 3)));
        const cr = Math.cos(rot), sr = Math.sin(rot);
        const at = (a) => { const ex = rx * Math.cos(a), ey = ry * Math.sin(a); return [cx + ex * cr - ey * sr, cy + ex * sr + ey * cr]; };
        const s = at(a0);
        if (this.cur) this.lineTo(m, s[0], s[1]); else this.moveTo(m, s[0], s[1]);
        for (let i = 1; i <= n; i++) { const p = at(a0 + sweep * i / n); this.lineTo(m, p[0], p[1]); }
      }
      arcTo(m, x1, y1, x2, y2, r) {
        if (r < 0) throw new DOMException('the radius is negative', 'IndexSizeError');
        this._resume(m);
        if (!this.last) { this.moveTo(m, x1, y1); return; }
        const [x0, y0] = this.last;
        const d0 = [x0 - x1, y0 - y1], d2 = [x2 - x1, y2 - y1];
        const l0 = Math.hypot(...d0), l2 = Math.hypot(...d2);
        const cross = d0[0] * d2[1] - d0[1] * d2[0];
        if (l0 === 0 || l2 === 0 || r === 0 || Math.abs(cross) < 1e-9) { this.lineTo(m, x1, y1); return; }
        const angle = Math.acos(Math.max(-1, Math.min(1, (d0[0] * d2[0] + d0[1] * d2[1]) / (l0 * l2))));
        const t = r / Math.tan(angle / 2);
        const p0 = [x1 + d0[0] / l0 * t, y1 + d0[1] / l0 * t], p2 = [x1 + d2[0] / l2 * t, y1 + d2[1] / l2 * t];
        const bis = [d0[0] / l0 + d2[0] / l2, d0[1] / l0 + d2[1] / l2], bl = Math.hypot(...bis);
        const h = r / Math.sin(angle / 2);
        const c = [x1 + bis[0] / bl * h, y1 + bis[1] / bl * h];
        const a0 = Math.atan2(p0[1] - c[1], p0[0] - c[0]), a1 = Math.atan2(p2[1] - c[1], p2[0] - c[0]);
        this.lineTo(m, p0[0], p0[1]);
        this.ellipse(m, c[0], c[1], r, r, 0, a0, a1, cross > 0);
      }
      rect(m, x, y, w, h) {
        this.moveTo(m, x, y); this.lineTo(m, x + w, y); this.lineTo(m, x + w, y + h); this.lineTo(m, x, y + h); this.closePath();
        this.last = [x, y]; this.start = [x, y];
      }
      roundRect(m, x, y, w, h, radii = 0) {
        const list = Array.isArray(radii) ? radii : [radii];
        const r = list.map(v => typeof v === 'object' && v ? Math.min(v.x ?? 0, v.y ?? 0) : +v || 0);
        const [tl, tr, br, bl] = r.length === 1 ? [r[0], r[0], r[0], r[0]] : r.length === 2 ? [r[0], r[1], r[0], r[1]]
                               : r.length === 3 ? [r[0], r[1], r[2], r[1]] : r.slice(0, 4);
        const k = Math.min(1, Math.abs(w) / Math.max(1e-9, tl + tr, bl + br), Math.abs(h) / Math.max(1e-9, tl + bl, tr + br));
        const q = (v) => Math.max(0, v * k);
        this.moveTo(m, x + q(tl), y);
        this.lineTo(m, x + w - q(tr), y); if (q(tr)) this.ellipse(m, x + w - q(tr), y + q(tr), q(tr), q(tr), 0, -Math.PI / 2, 0, false);
        this.lineTo(m, x + w, y + h - q(br)); if (q(br)) this.ellipse(m, x + w - q(br), y + h - q(br), q(br), q(br), 0, 0, Math.PI / 2, false);
        this.lineTo(m, x + q(bl), y + h); if (q(bl)) this.ellipse(m, x + q(bl), y + h - q(bl), q(bl), q(bl), 0, Math.PI / 2, Math.PI, false);
        this.lineTo(m, x, y + q(tl)); if (q(tl)) this.ellipse(m, x + q(tl), y + q(tl), q(tl), q(tl), 0, Math.PI, Math.PI * 1.5, false);
        this.closePath();
        this.last = [x, y]; this.start = [x, y];
      }
      // As the native side takes them.
      packed() {
        let total = 0;
        for (const s of this.subs) total += s.pts.length;
        const pts = new Float32Array(total), counts = new Int32Array(this.subs.length), closed = new Int32Array(this.subs.length);
        let at = 0;
        this.subs.forEach((s, i) => { pts.set(s.pts, at); at += s.pts.length; counts[i] = s.pts.length / 2; closed[i] = s.closed ? 1 : 0; });
        return { pts, counts, closed };
      }
      contains(x, y, evenOdd) {
        let wind = 0;
        for (const s of this.subs) {
          const p = s.pts, n = p.length / 2;
          for (let i = 0; i < n; i++) {
            const x0 = p[i * 2], y0 = p[i * 2 + 1], x1 = p[((i + 1) % n) * 2], y1 = p[((i + 1) % n) * 2 + 1];
            if ((y0 <= y) !== (y1 <= y)) {
              const cx = x0 + (y - y0) / (y1 - y0) * (x1 - x0);
              if (cx > x) wind += y1 > y0 ? 1 : -1;
            }
          }
        }
        return evenOdd ? (wind & 1) === 1 : wind !== 0;
      }
    }

    // Path2D: commands, replayed through whatever transform it's drawn under.
    const SVG_CMD = /([MmLlHhVvCcSsQqTtAaZz])([^MmLlHhVvCcSsQqTtAaZz]*)/g;
    class Path2D {
      constructor(from) {
        this._cmds = [];
        if (from instanceof Path2D) this._cmds = from._cmds.slice();
        else if (typeof from === 'string') this._svg(from);
      }
      _svg(d) {
        let x = 0, y = 0, sx = 0, sy = 0, lc = null, lq = null;
        for (const [, c, rest] of d.matchAll(SVG_CMD)) {
          const v = (rest.match(/-?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?/gi) || []).map(Number);
          const rel = c === c.toLowerCase(), C = c.toUpperCase();
          const take = (k, f) => { if (k === 0) { f([]); return; } for (let i = 0; i + k <= v.length; i += k) f(v.slice(i, i + k)); };
          if (C === 'Z') { this.closePath(); x = sx; y = sy; lc = lq = null; continue; }
          take({ M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2, A: 7 }[C], (a) => {
            const ox = rel ? x : 0, oy = rel ? y : 0;
            switch (C) {
              case 'M': x = ox + a[0]; y = oy + a[1]; sx = x; sy = y; this.moveTo(x, y); lc = lq = null; break;
              case 'L': x = ox + a[0]; y = oy + a[1]; this.lineTo(x, y); lc = lq = null; break;
              case 'H': x = (rel ? x : 0) + a[0]; this.lineTo(x, y); lc = lq = null; break;
              case 'V': y = (rel ? y : 0) + a[0]; this.lineTo(x, y); lc = lq = null; break;
              case 'C': this.bezierCurveTo(ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]); lc = [ox + a[2], oy + a[3]]; x = ox + a[4]; y = oy + a[5]; lq = null; break;
              case 'S': { const c1 = lc ? [2 * x - lc[0], 2 * y - lc[1]] : [x, y]; this.bezierCurveTo(c1[0], c1[1], ox + a[0], oy + a[1], ox + a[2], oy + a[3]); lc = [ox + a[0], oy + a[1]]; x = ox + a[2]; y = oy + a[3]; lq = null; break; }
              case 'Q': this.quadraticCurveTo(ox + a[0], oy + a[1], ox + a[2], oy + a[3]); lq = [ox + a[0], oy + a[1]]; x = ox + a[2]; y = oy + a[3]; lc = null; break;
              case 'T': { const q = lq ? [2 * x - lq[0], 2 * y - lq[1]] : [x, y]; this.quadraticCurveTo(q[0], q[1], ox + a[0], oy + a[1]); lq = q; x = ox + a[0]; y = oy + a[1]; lc = null; break; }
              case 'A': {
                // The SVG arc, through its centre (implementation notes F.6.5).
                const x2 = ox + a[5], y2 = oy + a[6];
                let rx = Math.abs(a[0]), ry = Math.abs(a[1]);
                const phi = a[2] * Math.PI / 180, fa = !!a[3], fs = !!a[4];
                if (!rx || !ry) { this.lineTo(x2, y2); x = x2; y = y2; break; }
                const cp = Math.cos(phi), sp = Math.sin(phi), dx = (x - x2) / 2, dy = (y - y2) / 2;
                const x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
                const lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
                if (lam > 1) { rx *= Math.sqrt(lam); ry *= Math.sqrt(lam); }
                const num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p, den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
                const k = (fa === fs ? -1 : 1) * Math.sqrt(Math.max(0, num / den));
                const cxp = k * rx * y1p / ry, cyp = -k * ry * x1p / rx;
                const cx = cp * cxp - sp * cyp + (x + x2) / 2, cy = sp * cxp + cp * cyp + (y + y2) / 2;
                const ang = (ux, uy, vx, vy) => Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
                const t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
                let dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
                if (!fs && dt > 0) dt -= 2 * Math.PI; else if (fs && dt < 0) dt += 2 * Math.PI;
                this.ellipse(cx, cy, rx, ry, phi, t1, t1 + dt, dt < 0);
                x = x2; y = y2; lc = lq = null;
                break;
              }
            }
          });
        }
      }
      addPath(p, t) {
        if (!(p instanceof Path2D)) throw new TypeError('addPath needs a Path2D');
        const m = t ? [t.a ?? t.m11 ?? 1, t.b ?? t.m12 ?? 0, t.c ?? t.m21 ?? 0, t.d ?? t.m22 ?? 1, t.e ?? t.m41 ?? 0, t.f ?? t.m42 ?? 0] : null;
        for (const c of p._cmds) this._cmds.push(m ? { op: 'withMatrix', m, cmd: c } : c);
      }
      _replay(b, m) {
        for (const c of this._cmds) {
          if (c.op === 'withMatrix') { b[c.cmd.op](mul(m, c.m), ...c.cmd.args); continue; }
          if (c.op === 'closePath') b.closePath(); else b[c.op](m, ...c.args);
        }
      }
    }
    for (const op of ['moveTo', 'lineTo', 'quadraticCurveTo', 'bezierCurveTo', 'arcTo', 'rect', 'roundRect', 'ellipse'])
      Path2D.prototype[op] = function (...args) { if (args.slice(0, op === 'roundRect' ? 4 : args.length).every(v => typeof v !== 'number' || Number.isFinite(v))) this._cmds.push({ op, args }); };
    Path2D.prototype.arc = function (x, y, r, a0, a1, ccw = false) { this._cmds.push({ op: 'ellipse', args: [x, y, r, r, 0, a0, a1, !!ccw] }); };
    Path2D.prototype.closePath = function () { this._cmds.push({ op: 'closePath', args: [] }); };

    class CanvasGradient {
      constructor(kind, coords) { this._kind = kind; this._coords = coords; this._stops = []; }
      addColorStop(offset, color) {
        offset = +offset;
        if (!(offset >= 0 && offset <= 1)) throw new DOMException('the offset is out of range', 'IndexSizeError');
        this._stops.push([offset, String(color)]);
      }
    }
    class CanvasPattern { setTransform() {} }

    class ImageData {
      constructor(a, b, c) {
        if (a instanceof Uint8ClampedArray) {
          const w = b >>> 0, h = c === undefined ? a.length / 4 / w : c >>> 0;
          if (!w || a.length !== w * h * 4) throw new DOMException('the data is the wrong size', 'IndexSizeError');
          this.data = a; this.width = w; this.height = h;
        } else {
          const w = a >>> 0, h = b >>> 0;
          if (!w || !h) throw new DOMException('the size is zero', 'IndexSizeError');
          this.data = new Uint8ClampedArray(w * h * 4); this.width = w; this.height = h;
        }
        this.colorSpace = 'srgb';
      }
    }

    const CAPS = { butt: 0, round: 1, square: 2 }, JOINS = { miter: 0, round: 1, bevel: 2 };
    const fresh = () => ({ m: [1, 0, 0, 1, 0, 0], fill: '#000000', stroke: '#000000', alpha: 1, lineWidth: 1, cap: 'butt', join: 'miter',
                           miter: 10, dash: [], dashOffset: 0, font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic',
                           direction: 'inherit', composite: 'source-over', smoothing: true, smoothingQuality: 'low', shadowBlur: 0,
                           shadowColor: 'rgba(0, 0, 0, 0)', shadowOffsetX: 0, shadowOffsetY: 0, filter: 'none', letterSpacing: '0px',
                           clips: [] });
    const styleValue = (v) => {
      if (v instanceof CanvasGradient) return { kind: v._kind, coords: new Float32Array(v._coords), stops: v._stops };
      return v instanceof CanvasPattern ? '#000000' : v;
    };
    const fontSize = (font) => { const m = /(\d*\.?\d+)px/.exec(font); return m ? +m[1] : 10; };

    class CanvasRenderingContext2D {
      constructor(canvas, key) {
        if (key !== ctxKey) throw new TypeError('Illegal constructor');
        this.canvas = canvas; this._s = fresh(); this._stack = []; this._path = new Builder();
      }
      // State.
      save() { const s = this._s; this._stack.push(Object.assign({}, s, { m: s.m.slice(), dash: s.dash.slice(), clips: s.clips.slice() })); }
      restore() {
        if (!this._stack.length) return;
        const had = this._s.clips.length;
        this._s = this._stack.pop();
        if (had !== this._s.clips.length) this._reclip();
      }
      reset() { this._s = fresh(); this._stack = []; this._path = new Builder(); N.reset(this.canvas); }
      isContextLost() { return false; }
      getContextAttributes() { return { alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: false }; }
      _reclip() {
        N.unclip(this.canvas);
        for (const c of this._s.clips) N.clip(this.canvas, c.pts, c.counts, c.evenOdd);
      }
      // Transforms.
      scale(x, y) { if (finite(x, y)) this._s.m = mul(this._s.m, [x, 0, 0, y, 0, 0]); }
      rotate(a) { if (finite(a)) { const c = Math.cos(a), s = Math.sin(a); this._s.m = mul(this._s.m, [c, s, -s, c, 0, 0]); } }
      translate(x, y) { if (finite(x, y)) this._s.m = mul(this._s.m, [1, 0, 0, 1, x, y]); }
      transform(a, b, c, d, e, f) { if (finite(a, b, c, d, e, f)) this._s.m = mul(this._s.m, [a, b, c, d, e, f]); }
      setTransform(a, b, c, d, e, f) {
        if (a && typeof a === 'object') { const t = a; this._s.m = [t.a ?? t.m11 ?? 1, t.b ?? t.m12 ?? 0, t.c ?? t.m21 ?? 0, t.d ?? t.m22 ?? 1, t.e ?? t.m41 ?? 0, t.f ?? t.m42 ?? 0]; return; }
        if (a === undefined) { this._s.m = [1, 0, 0, 1, 0, 0]; return; }
        if (finite(a, b, c, d, e, f)) this._s.m = [a, b, c, d, e, f];
      }
      resetTransform() { this._s.m = [1, 0, 0, 1, 0, 0]; }
      getTransform() {
        const [a, b, c, d, e, f] = this._s.m;
        return typeof DOMMatrix === 'function' ? new DOMMatrix([a, b, c, d, e, f]) : { a, b, c, d, e, f, is2D: true, isIdentity: a === 1 && !b && !c && d === 1 && !e && !f };
      }
      // Styles.
      get fillStyle() { return this._s.fill; }
      set fillStyle(v) { if (typeof v === 'string' || v instanceof CanvasGradient || v instanceof CanvasPattern) this._s.fill = v; }
      get strokeStyle() { return this._s.stroke; }
      set strokeStyle(v) { if (typeof v === 'string' || v instanceof CanvasGradient || v instanceof CanvasPattern) this._s.stroke = v; }
      get globalAlpha() { return this._s.alpha; }
      set globalAlpha(v) { v = +v; if (v >= 0 && v <= 1) this._s.alpha = v; }
      get lineWidth() { return this._s.lineWidth; }
      set lineWidth(v) { v = +v; if (v > 0 && Number.isFinite(v)) this._s.lineWidth = v; }
      get lineCap() { return this._s.cap; }
      set lineCap(v) { if (v in CAPS) this._s.cap = v; }
      get lineJoin() { return this._s.join; }
      set lineJoin(v) { if (v in JOINS) this._s.join = v; }
      get miterLimit() { return this._s.miter; }
      set miterLimit(v) { v = +v; if (v > 0 && Number.isFinite(v)) this._s.miter = v; }
      setLineDash(d) { if (Array.isArray(d) && d.every(x => Number.isFinite(+x) && +x >= 0)) this._s.dash = (d.length % 2 ? d.concat(d) : d.slice()).map(Number); }
      getLineDash() { return this._s.dash.slice(); }
      get lineDashOffset() { return this._s.dashOffset; }
      set lineDashOffset(v) { if (Number.isFinite(+v)) this._s.dashOffset = +v; }
      createLinearGradient(x0, y0, x1, y1) { return new CanvasGradient('linear', [x0, y0, x1, y1]); }
      createRadialGradient(x0, y0, r0, x1, y1, r1) {
        if (r0 < 0 || r1 < 0) throw new DOMException('the radius is negative', 'IndexSizeError');
        return new CanvasGradient('radial', [x0, y0, r0, x1, y1, r1]);
      }
      createConicGradient(a, x, y) { return new CanvasGradient('conic', [a, x, y]); }
      createPattern() { return new CanvasPattern(); }
      // Paths.
      beginPath() { this._path = new Builder(); }
      closePath() { this._path.closePath(); }
      moveTo(x, y) { if (finite(x, y)) this._path.moveTo(this._s.m, x, y); }
      lineTo(x, y) { if (finite(x, y)) this._path.lineTo(this._s.m, x, y); }
      quadraticCurveTo(a, b, x, y) { if (finite(a, b, x, y)) this._path.quadraticCurveTo(this._s.m, a, b, x, y); }
      bezierCurveTo(a, b, c, d, x, y) { if (finite(a, b, c, d, x, y)) this._path.bezierCurveTo(this._s.m, a, b, c, d, x, y); }
      arc(x, y, r, a0, a1, ccw = false) { if (finite(x, y, r, a0, a1)) this._path.ellipse(this._s.m, x, y, r, r, 0, a0, a1, !!ccw); }
      arcTo(x1, y1, x2, y2, r) { if (finite(x1, y1, x2, y2, r)) this._path.arcTo(this._s.m, x1, y1, x2, y2, r); }
      ellipse(x, y, rx, ry, rot, a0, a1, ccw = false) { if (finite(x, y, rx, ry, rot, a0, a1)) this._path.ellipse(this._s.m, x, y, rx, ry, rot, a0, a1, !!ccw); }
      rect(x, y, w, h) { if (finite(x, y, w, h)) this._path.rect(this._s.m, x, y, w, h); }
      roundRect(x, y, w, h, r) { if (finite(x, y, w, h)) this._path.roundRect(this._s.m, x, y, w, h, r); }
      _shape(path) {
        if (path instanceof Path2D) { const b = new Builder(); path._replay(b, this._s.m); return b; }
        return this._path;
      }
      _dashed(b) {
        const dash = this._s.dash;
        if (!dash.length || dash.every(d => d === 0)) return b;
        // Dashes along each subpath, in canvas pixels.
        const k = scaleOf(this._s.m), out = new Builder(), I = [1, 0, 0, 1, 0, 0];
        for (const s of b.subs) {
          const p = s.closed ? s.pts.concat(s.pts.slice(0, 2)) : s.pts;
          let di = 0, left = dash[0] * k, on = true;
          let off = (this._s.dashOffset * k) % (dash.reduce((x, y) => x + y, 0) * k);
          while (off > 0) { const take = Math.min(off, left); off -= take; left -= take; if (left <= 0) { di = (di + 1) % dash.length; left = dash[di] * k; on = !on; } }
          if (on) out.moveTo(I, p[0], p[1]);
          for (let i = 2; i < p.length; i += 2) {
            let x0 = p[i - 2], y0 = p[i - 1];
            const x1 = p[i], y1 = p[i + 1];
            let seg = Math.hypot(x1 - x0, y1 - y0);
            while (seg > 0) {
              const take = Math.min(seg, left), t = take / seg;
              const nx = x0 + (x1 - x0) * t, ny = y0 + (y1 - y0) * t;
              if (on) out.lineTo(I, nx, ny);
              x0 = nx; y0 = ny; seg -= take; left -= take;
              if (left <= 1e-9) { di = (di + 1) % dash.length; left = dash[di] * k; on = !on; if (on) out.moveTo(I, x0, y0); else out.cur = null; }
            }
          }
        }
        return out;
      }
      fill(a, b) {
        const path = a instanceof Path2D ? a : null, rule = path ? b : a;
        const { pts, counts } = this._shape(path).packed();
        if (counts.length) N.fill(this.canvas, pts, counts, rule === 'evenodd', styleValue(this._s.fill), new Float32Array(this._s.m), this._s.alpha);
      }
      stroke(path) {
        const { pts, counts, closed } = this._dashed(this._shape(path instanceof Path2D ? path : null)).packed();
        if (counts.length)
          N.stroke(this.canvas, pts, counts, closed, this._s.lineWidth * scaleOf(this._s.m), CAPS[this._s.cap], JOINS[this._s.join], this._s.miter,
                   styleValue(this._s.stroke), new Float32Array(this._s.m), this._s.alpha);
      }
      clip(a, b) {
        const path = a instanceof Path2D ? a : null, rule = path ? b : a;
        const { pts, counts } = this._shape(path).packed();
        const c = { pts, counts, evenOdd: rule === 'evenodd' };
        this._s.clips = this._s.clips.concat([c]);
        N.clip(this.canvas, pts, counts, c.evenOdd);
      }
      isPointInPath(a, b, c, d) {
        const path = a instanceof Path2D ? a : null;
        const [x, y, rule] = path ? [b, c, d] : [a, b, c];
        return this._shape(path).contains(+x, +y, rule === 'evenodd');
      }
      isPointInStroke() { return false; }
      // Rectangles.
      fillRect(x, y, w, h) {
        if (!finite(x, y, w, h)) return;
        const b = new Builder(); b.rect(this._s.m, x, y, w, h);
        const { pts, counts } = b.packed();
        N.fill(this.canvas, pts, counts, false, styleValue(this._s.fill), new Float32Array(this._s.m), this._s.alpha);
      }
      strokeRect(x, y, w, h) {
        if (!finite(x, y, w, h)) return;
        const saved = this._path; this._path = new Builder(); this._path.rect(this._s.m, x, y, w, h);
        this.stroke(); this._path = saved;
      }
      clearRect(x, y, w, h) {
        if (!finite(x, y, w, h)) return;
        const m = this._s.m, xs = [], ys = [];
        for (const [px, py] of [[x, y], [x + w, y], [x, y + h], [x + w, y + h]]) { xs.push(m[0] * px + m[2] * py + m[4]); ys.push(m[1] * px + m[3] * py + m[5]); }
        const x0 = Math.min(...xs), y0 = Math.min(...ys);
        N.clearRect(this.canvas, x0, y0, Math.max(...xs) - x0, Math.max(...ys) - y0);
      }
      // Pixels.
      createImageData(a, b) { return a instanceof ImageData ? new ImageData(a.width, a.height) : new ImageData(Math.abs(a | 0), Math.abs(b | 0)); }
      getImageData(x, y, w, h) {
        if (!w || !h) throw new DOMException('the size is zero', 'IndexSizeError');
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        const buf = N.read(this.canvas, x | 0, y | 0, w | 0, h | 0);
        return new ImageData(new Uint8ClampedArray(buf), w | 0, h | 0);
      }
      putImageData(img, dx, dy, sx, sy, sw, sh) {
        if (!(img instanceof ImageData)) throw new TypeError('putImageData needs ImageData');
        if (sx === undefined) { N.write(this.canvas, img.data, img.width, img.height, dx | 0, dy | 0); return; }
        // Only the dirty rectangle: copy it out first.
        sx |= 0; sy |= 0; sw |= 0; sh |= 0;
        if (sw < 0) { sx += sw; sw = -sw; }
        if (sh < 0) { sy += sh; sh = -sh; }
        sx = Math.max(0, sx); sy = Math.max(0, sy); sw = Math.min(sw, img.width - sx); sh = Math.min(sh, img.height - sy);
        if (sw <= 0 || sh <= 0) return;
        const part = new Uint8ClampedArray(sw * sh * 4);
        for (let r = 0; r < sh; r++) part.set(img.data.subarray(((sy + r) * img.width + sx) * 4, ((sy + r) * img.width + sx + sw) * 4), r * sw * 4);
        N.write(this.canvas, part, sw, sh, (dx | 0) + sx, (dy | 0) + sy);
      }
      // Pictures: other canvases (images aren't drawn yet).
      drawImage(src, ...a) {
        const isImage = typeof HTMLImageElement === 'function' && src instanceof HTMLImageElement;
        const isBitmap = src && src._xwebBitmapOf instanceof HTMLCanvasElement;
        if (isBitmap) src = src._xwebBitmapOf;
        if (!(src instanceof HTMLCanvasElement) && !isImage) {
          if (src && (src instanceof SVGElement || (typeof HTMLVideoElement === 'function' && src instanceof HTMLVideoElement))) return;
          throw new TypeError('drawImage needs an image or a canvas');
        }
        const url = isImage ? (src.currentSrc || src.src) : null;
        const [sw0, sh0] = isImage ? N.imageSize(url) : N.size(src);
        if (isImage && (!sw0 || !sh0)) return;   // not loaded (yet): nothing to draw
        let sx = 0, sy = 0, sw = sw0, sh = sh0, dx, dy, dw, dh;
        if (a.length <= 2) [dx, dy] = a, dw = sw, dh = sh;
        else if (a.length <= 4) [dx, dy, dw, dh] = a;
        else [sx, sy, sw, sh, dx, dy, dw, dh] = a;
        if (!finite(sx, sy, sw, sh, dx, dy, dw, dh) || !sw || !sh) return;
        // Source pixels onto the destination rectangle, then the current transform.
        const fit = [dw / sw, 0, 0, dh / sh, dx - sx * dw / sw, dy - sy * dh / sh];
        const m = new Float32Array(mul(this._s.m, fit));
        if (sx || sy || sw !== sw0 || sh !== sh0) {
          // Only part of the source: clip to where it lands.
          this.save();
          const b = new Builder(); b.rect(this._s.m, dx, dy, dw, dh);
          const { pts, counts } = b.packed();
          this._s.clips = this._s.clips.concat([{ pts, counts, evenOdd: false }]);
          N.clip(this.canvas, pts, counts, false);
          if (isImage) N.drawImage(this.canvas, url, m, this._s.alpha); else N.drawCanvas(this.canvas, src, m, this._s.alpha);
          this.restore();
          return;
        }
        if (isImage) N.drawImage(this.canvas, url, m, this._s.alpha); else N.drawCanvas(this.canvas, src, m, this._s.alpha);
      }
      // Text: measured roughly; not drawn yet.
      get font() { return this._s.font; }
      set font(v) { if (typeof v === 'string' && /\d/.test(v)) this._s.font = v; }
      get textAlign() { return this._s.textAlign; }
      set textAlign(v) { if (['start', 'end', 'left', 'right', 'center'].includes(v)) this._s.textAlign = v; }
      get textBaseline() { return this._s.textBaseline; }
      set textBaseline(v) { if (['top', 'hanging', 'middle', 'alphabetic', 'ideographic', 'bottom'].includes(v)) this._s.textBaseline = v; }
      measureText(text) {
        const size = fontSize(this._s.font);
        const got = N.measure(String(text), this._s.font);
        if (got) {
          const [w, asc, desc] = got;
          const left = this._s.textAlign === 'center' ? w / 2 : (this._s.textAlign === 'right' || this._s.textAlign === 'end') ? w : 0;
          return { width: w, actualBoundingBoxLeft: left, actualBoundingBoxRight: w - left, actualBoundingBoxAscent: asc, actualBoundingBoxDescent: desc,
                   fontBoundingBoxAscent: asc, fontBoundingBoxDescent: desc, emHeightAscent: asc, emHeightDescent: desc,
                   hangingBaseline: asc * 0.8, alphabeticBaseline: 0, ideographicBaseline: -desc };
        }
        let w = 0;
        for (const ch of String(text)) w += (ch.codePointAt(0) > 0x2e80 ? 1 : /[il.,:;'|!]/.test(ch) ? 0.28 : /[mwMW@]/.test(ch) ? 0.85 : 0.55) * size;
        return { width: w, actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, actualBoundingBoxAscent: size * 0.72, actualBoundingBoxDescent: size * 0.2,
                 fontBoundingBoxAscent: size * 0.9, fontBoundingBoxDescent: size * 0.25, emHeightAscent: size * 0.8, emHeightDescent: size * 0.2,
                 hangingBaseline: size * 0.7, alphabeticBaseline: 0, ideographicBaseline: -size * 0.2 };
      }
      _text(text, x, y, maxWidth, style) {
        if (!finite(x, y)) return;
        const align = { left: -1, start: -1, center: 0, right: 1, end: 1 }[this._s.textAlign] ?? -1;
        const shift = { alphabetic: 0, ideographic: -1, bottom: -1, top: 1, hanging: 0.8, middle: 0.5 }[this._s.textBaseline] ?? 0;
        N.text(this.canvas, String(text).replace(/[\t\n\f\r]/g, ' '), this._s.font, x, y, align, shift, new Float32Array(this._s.m),
               styleValue(style), this._s.alpha, maxWidth === undefined ? NaN : +maxWidth);
      }
      fillText(text, x, y, maxWidth) { this._text(text, x, y, maxWidth, this._s.fill); }
      // Outlines of glyphs aren't drawn: their filling, in the stroke's colour, stands in.
      strokeText(text, x, y, maxWidth) { this._text(text, x, y, maxWidth, this._s.stroke); }
      drawFocusIfNeeded() {}
      scrollPathIntoView() {}
    }
    const ctxKey = {};
    for (const [k, def] of [['direction', 'inherit'], ['globalCompositeOperation', 'composite'], ['imageSmoothingEnabled', 'smoothing'],
                            ['imageSmoothingQuality', 'smoothingQuality'], ['shadowBlur', 'shadowBlur'], ['shadowColor', 'shadowColor'],
                            ['shadowOffsetX', 'shadowOffsetX'], ['shadowOffsetY', 'shadowOffsetY'], ['filter', 'filter'], ['letterSpacing', 'letterSpacing']]) {
      const key = def === 'inherit' ? 'direction' : def;
      Object.defineProperty(CanvasRenderingContext2D.prototype, k, { get() { return this._s[key]; }, set(v) { this._s[key] = v; }, configurable: true, enumerable: true });
    }

    const contexts = new WeakMap();
    const P = HTMLCanvasElement.prototype;
    P.getContext = function getContext(type) {
      if (type !== '2d') return null;
      let c = contexts.get(this);
      if (!c) { c = new CanvasRenderingContext2D(this, ctxKey); contexts.set(this, c); }
      return c;
    };
    for (const [dim, fallback] of [['width', 300], ['height', 150]])
      Object.defineProperty(P, dim, {
        get() { const v = parseInt(this.getAttribute(dim), 10); return v >= 0 ? v : fallback; },
        set(v) {
          this.setAttribute(dim, String(Math.max(0, v >>> 0)));
          const c = contexts.get(this);
          if (c) { c._s = fresh(); c._stack = []; c._path = new Builder(); }
          N.reset(this);
        },
        configurable: true, enumerable: true });
    P.toDataURL = function toDataURL() { return N.dataUrl(this); };
    P.toBlob = function toBlob(cb, type) {
      const bytes = N.png(this);
      setTimeout(() => cb(new Blob([bytes], { type: 'image/png' })), 0);
    };
    P.captureStream = function () { throw new DOMException('canvas streams are not supported', 'NotSupportedError'); };
    for (const [name, value] of [['CanvasRenderingContext2D', CanvasRenderingContext2D], ['Path2D', Path2D], ['CanvasGradient', CanvasGradient],
                                 ['CanvasPattern', CanvasPattern], ['ImageData', ImageData]])
      Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
  }

  // FontFace and document.fonts. The page's web fonts are loaded by the renderer, not
  // through here; to scripts every face is loaded, and asking to load one answers with
  // a face for that family at once (FontFaceObserver and friends wait on this).
  class FontFace {
    constructor(family, source, descriptors = {}) {
      this.family = String(family).replace(/^["']|["']$/g, '');
      this.style = descriptors.style || 'normal';
      this.weight = String(descriptors.weight || 'normal');
      this.stretch = descriptors.stretch || 'normal';
      this.unicodeRange = descriptors.unicodeRange || 'U+0-10FFFF';
      this.display = descriptors.display || 'auto';
      this.featureSettings = descriptors.featureSettings || 'normal';
      this.status = 'loaded';
      this.loaded = Promise.resolve(this);
    }
    load() { return Promise.resolve(this); }
  }
  {
    const familiesOf = (font) => {
      // The family list at the end of a font shorthand ("italic 700 16px 'A B', serif").
      const m = /(?:^|\s)(?:[\d.]+(?:px|pt|em|rem|%)|xx?-small|small|medium|large|xx?-large)(?:\/\S+)?\s+(.+)$/.exec(String(font).trim());
      const list = m ? m[1] : String(font);
      return list.split(',').map(f => f.trim().replace(/^["']|["']$/g, '')).filter(Boolean);
    };
    const added = new Set();
    const fonts = new (class FontFaceSet extends EventTarget {
      constructor() { super(); this.status = 'loaded'; this.onloading = this.onloadingdone = this.onloadingerror = null; }
      get ready() { return Promise.resolve(this); }
      get size() { return added.size; }
      load(font) { const fam = familiesOf(font)[0]; return Promise.resolve(fam ? [new FontFace(fam, '')] : []); }
      check() { return true; }
      add(face) { added.add(face); return this; }
      delete(face) { return added.delete(face); }
      has(face) { return added.has(face); }
      clear() { added.clear(); }
      forEach(cb, thisArg) { for (const f of added) cb.call(thisArg, f, f, this); }
      values() { return added.values(); }
      keys() { return added.values(); }
      entries() { return added.entries(); }
      [Symbol.iterator]() { return added.values(); }
    })();
    Object.defineProperty(document, 'fonts', { value: fonts, configurable: true, enumerable: true });
    Object.defineProperty(global, 'FontFace', { value: FontFace, writable: true, configurable: true });
    Object.defineProperty(global, 'FontFaceSet', { value: fonts.constructor, writable: true, configurable: true });
  }
  // window.postMessage, to this window (the only one a page here has): a "message"
  // event as a task, if the target origin allows. Schedulers use it to yield.
  {
    const post = function postMessage(data, targetOrigin, transfer) {
      let target = targetOrigin;
      if (targetOrigin && typeof targetOrigin === 'object') target = targetOrigin.targetOrigin;
      target = target === undefined ? '/' : String(target);
      const origin = location.origin;
      if (target !== '*' && target !== '/') {
        let wanted;
        try { wanted = new URL(target).origin; } catch (e) { throw new DOMException('the target origin is not a valid URL', 'SyntaxError'); }
        if (wanted !== origin) return;
      }
      const copy = structuredClone(data);
      setTimeout(() => {
        const e = new MessageEvent('message', { data: copy, origin, source: global, lastEventId: '', ports: [] });
        global.dispatchEvent(e);
      }, 0);
    };
    Object.defineProperty(global, 'postMessage', { value: post, writable: true, configurable: true, enumerable: true });
  }
  for (const [name, value] of [['MessageChannel', MessageChannel], ['MessagePort', MessagePort], ['BroadcastChannel', BroadcastChannel]])
    if (!global[name]) Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });

  for (const [name, value] of [['Blob', Blob], ['File', File], ['FormData', FormData], ['Headers', Headers], ['Request', Request],
                               ['Response', Response], ['fetch', fetch], ['XMLHttpRequest', XMLHttpRequest],
                               ['structuredClone', structuredClone], ['requestIdleCallback', requestIdleCallback],
                               ['cancelIdleCallback', cancelIdleCallback], ['IntersectionObserver', IntersectionObserver],
                               ['ResizeObserver', ResizeObserver], ['DOMException', DOMException],
                               ['PerformanceObserver', PerformanceObserver], ['Intl', Intl]])
    Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
  for (const [name, value] of [['URL', URL], ['URLSearchParams', URLSearchParams], ['AbortController', AbortController],
                               ['AbortSignal', AbortSignal], ['NodeFilter', NodeFilter], ['TreeWalker', TreeWalker],
                               ['NodeIterator', NodeIterator]])
    Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
})(this);
)JS";

}  // namespace fbx::xweb
