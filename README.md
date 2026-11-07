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
| Network (http, https, `data:`), links, redirects, history, fragments, UTF-8, UTF-16, Latin-1 | **JavaScript, cookies**, other encodings (Shift_JIS, EUC-KR...), HTTP cache |
| **Forms**: text fields (typing, the input method, caret, paste), passwords, checkboxes, radio groups, labels, drop-down selects, textareas, submit and reset; GET, and POST as form-urlencoded, multipart or text/plain; Enter submits; Tab between fields; controls drawn like Chromium's | file uploads, `<input type=date>` and other pickers, validation (`required`, `pattern`), selecting text with the mouse, autofill, multi-line wrapping in textareas, `<select multiple>` changed by clicking |
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
