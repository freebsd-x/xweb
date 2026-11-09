# xweb

xweb — FreeBSD-X's own web engine. The HTML and CSS parsers, style engine and layout are
written by hand here, with no parser or engine libraries.

Part of **FreeBSD-X**, a GUI platform for FreeBSD. The full workspace is a multi-repo
project assembled with the [`repo`](https://gerrit.googlesource.com/git-repo) tool:

```sh
repo init -u https://github.com/freebsd-x/manifest.git
repo sync
```

## Where it stands

The Browser app runs on this engine (`out/xwebview [address or file]` is the same viewer
on its own): it follows links and redirects, has history and an address bar, and fetches
pages, stylesheets, images and fonts through the compositor's broker from inside its
sandbox. Many sites read well; anything needing scripts doesn't work yet, and the
Browser's "Open in Chromium" hands such a page to Chromium.

| Works | Not yet |
|---|---|
| HTML5 parsing, including error recovery, templates, SVG/MathML trees | `<selectedcontent>` mirroring |
| CSS parsing, selectors (not `:has()`), cascade, `@media` with range syntax and `calc()`, `@supports`, `@import`, `var()`, `calc()`, `::before`/`::after`, `display: contents`, `light-dark()` | animations, transforms, `@container`, masks over anything but a background colour, gradient masks |
| Block and inline layout, margin collapsing, inline-block, lists, `width: max-content` and friends | form controls' widths are approximate, especially with an inherited font |
| **Floats, tables (auto and fixed), flexbox, grid** (areas, named lines, `repeat`, `minmax`, `fr`, auto-placement), positioning | subgrid, masonry, baseline alignment and absolute positioning within grid areas; sticky, z-index stacking contexts, collapsed-border conflict resolution |
| **Web fonts**: `@font-face`, WOFF, WOFF2, TTF, OTF, variable weights, `unicode-range`, fonts in `data:` URLs, swapped in as they arrive | synthetic bold and italic, cross-origin checks on fonts, `font-display`, other variation axes, lines mixing fonts growing to fit all of them under `line-height: normal` |
| **Backgrounds**: colours, images (`url()`, layers, size, position, repeat, origin, clip), linear, radial and conic gradients (repeating too); masked icons; borders in every style (dashed, dotted, double, groove, ridge, inset, outset), corners each their own and elliptical, `box-shadow` (blur, spread, inset, several), overflow clipping (positioned children too), text, images | `text-shadow`, `outline`, `background-attachment: fixed`, `space`/`round` repeats, rounded clipping of background images and of overflow; dashes round a rounded corner are spaced a little differently from Chromium's |
| **SVG**, inline and as images: paths, shapes, groups, `<use>`/`<symbol>`, transforms, `viewBox`, gradients, clip paths, strokes with joins, caps and dashes, group opacity, CSS styling | SVG text, `<image>`, masks, patterns, markers, filters |
| Network (http, https, `data:`), links, redirects, history, fragments, UTF-8, UTF-16, Latin-1 | `localStorage` kept past the session and other storage scripts use, other encodings (Shift_JIS, EUC-KR...), HTTP cache, a way to clear cookies from the Browser |
| **Forms**: text fields (typing, the input method, caret, paste), passwords, checkboxes, radio groups, labels, drop-down selects, textareas, submit and reset; GET, and POST as form-urlencoded, multipart or text/plain; Enter submits; Tab between fields; controls drawn like Chromium's | file uploads, `<input type=date>` and other pickers, validation (`required`, `pattern`), selecting text with the mouse, autofill, multi-line wrapping in textareas, `<select multiple>` changed by clicking |
| **Cookies**: kept by xfetch with curl's engine (the public suffix list, expiry, paths, `Secure`), sent with pages, forms and the page's own site's files, never with another site's (no third-party tracking), saved in `~/.local/share/freebsdx/cookies.txt` for the user alone | `SameSite` (a cross-site form's POST still carries cookies), per-site settings |
| **JavaScript engine** (`src/js`, `out/sdk/xweb/xjs [--module] file.js`), our own: the ES2022 syntax, closures, `let`/`const` with their dead zone, classes with fields and `super`, destructuring, spread, optional chaining, template literals, exceptions, getters and setters; Object, Function, Array, String, Number, Boolean, Symbol, Math, JSON, Error types, Map, Set, iterators, **Proxy and Reflect** (`src/js/proxy.cc`: every trap, with the invariant checks), **BigInt** (`src/js/bigint.cc`, our own arbitrary-precision arithmetic; BigInt64Array, BigUint64Array, DataView's 64-bit accessors), Object.groupBy, console; **regular expressions** (`src/js/regexp.cc`, our own backtracking matcher: named groups, lookahead and lookbehind, backreferences, the `u` mode with `\p{…}`, sticky and `d` indices, the web's legacy syntax), with String's `match`, `matchAll`, `replace`, `search` and `split`; **Promise** and the microtask queue, **generators, async functions, async generators** and `for await` (each body on a coroutine of its own: `src/js/coroutine.cc`, a hand-written stack switch, suspended stacks scanned by the collector), **ES modules** (`src/js/modules.cc`: `<script type=module>`, import maps, live bindings, cycles, top-level await, `import()`, `import.meta`), WeakMap, WeakSet, WeakRef; **Date** (`src/js/date.cc`: the local zone through libc, ISO and loose parsing); **ArrayBuffer, the typed arrays, DataView**, TextEncoder and TextDecoder (`src/js/typedarray.cc`); **`eval`** (direct and indirect) and `new Function`; the language's early errors; in pages, **`fetch`** and XMLHttpRequest with **CORS** (`src/script_fetch.cc`: any method, the page's own headers, response headers back; cross-origin reads need the server's Access-Control-Allow-Origin, preflights for what isn't simple, credentials only when asked and allowed, opaque no-cors responses; a page on the web can't reach this machine or the local network), Headers, Request, Response, Blob, FormData, URL, URLSearchParams, AbortController and TreeWalker (`src/script_prelude.cc`); limits on time, memory and nesting; scripts run on a large stack of the engine's own, so recursion goes about as deep as in other browsers | `with`, Intl beyond English (pages get an en-US Intl); fetch's streaming bodies, HTTP caching rules; the `v` flag's set syntax, and the full Unicode tables (`\p{…}` and case folding cover the common scripts by block) |
| **Scripts on pages** (`src/script.cc`, `src/script_dom.cc`): `<script>`s run in order, inline and external; `document` (querying with selectors, making and changing elements, `innerHTML`, `title`, `write`), Node and Element (tree, attributes, `classList`, `style`, `dataset` to read, geometry from layout, form values), events with capture and bubbling (click, input, change, submit, keydown, DOMContentLoaded, load), `setTimeout`, `setInterval`, `requestAnimationFrame`, `queueMicrotask`, `location`, `history.pushState`, `localStorage` for the session, `matchMedia`, `getComputedStyle` (part); an interface per element (HTMLDivElement and the rest), **custom elements** (`customElements.define`, upgrades, lifecycle and attribute callbacks), **shadow DOM** (`attachShadow`, declarative `<template shadowrootmode>`, slots; styles scoped to their tree with `:host`, `::slotted()` and `:defined`, rendered as the composed tree), MutationObserver, constructible stylesheets and `adoptedStyleSheets`, `<template>.content`, DocumentFragment, DOMException, `structuredClone`, `CSS.supports` and `CSS.escape`, IntersectionObserver and ResizeObserver (each target reported once), `requestIdleCallback`, `crypto.getRandomValues` and `randomUUID`, an English Intl (NumberFormat, DateTimeFormat, RelativeTimeFormat, PluralRules, ListFormat, Collator, Segmenter) (`src/script_dom.cc`, `src/script_prelude.cc`) | `document.cookie`, events retargeted across shadow boundaries, `slotchange`, iframes, workers, canvas drawing, media playback, mouse movement and hover events, `dataset` writes |
| Refuses to reach this machine or the local network | so a LAN device's page can't be opened either |

## Layers

- `dom.h`, `html_parser.h`: the tree and the WHATWG parser. No dependencies.
- `css.h`: CSS Syntax 3 tokenizer, rules, selectors, media queries.
- `style.h`: matching, cascade, computed values.
- `layout.h`: boxes and paint operations, asking a `TextMeasurer` about text.
- `url.h`, `encoding.h`: addresses and character sets.
- `text_measurer.h`, `tools/xweb_view.cc`: the only parts that know about xrender and xkit.

Pages are untrusted input. Depth, selector matching, `var()` expansion, imports, grid
placement and fonts all have limits, font files reach only FreeType inside the sandbox, and nothing a page does can make the parsers fail.

## Tests

- `xweb html5lib`: the html5lib tree-construction suite (`tests/html5lib`, MIT). Gaps are
  listed in `tests/html5lib_known_failures.txt`; a listed test that starts passing fails.
- `xweb css`, `style`, `layout`, `html`, `paint`: unit tests (`paint` is the rasterizer,
  gradients and SVG).
- `xweb fuzz`: mutated and pathological input through the whole pipeline. Worth running
  in an ASan build after changing the engine.
- `scripts/xweb-compare.py sdk/xweb/tests/pages/*.html`: every element's box against
  headless Chromium. Needs Chromium, so it isn't in the unit suite.
- `scripts/xweb-pixels.py sdk/xweb/tests/pages/*.html`: the first screenful painted on the
  CPU by `xweb_paint`, against Chromium's screenshot, pixel by pixel with text hidden on both
  sides. `XWEB_PAINT_OPS=1 out/sdk/xweb/xweb_paint page.html w h out.png` lists what's painted.
- `headless-e2e.py webview webnav webfont webpaint`: a page in a real window checked pixel
  by pixel, browsing a local test site, including that nobody else may browse, a page's own
  fonts arriving through the sandbox, and gradients, SVG, CSS images and masks.
- `tests/make_webfonts.py`: makes the WOFF fonts `tests/pages/webfont.html` uses from
  installed ones, as the fonts aren't ours to keep here. `xweb-compare.py` runs it. WOFF2 has
  no repeatable test: it's FreeType's to decode, and was checked by hand against Chromium
  with fonts from Google Fonts, including a variable one.
