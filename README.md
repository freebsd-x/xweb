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

The Browser app still runs Chromium. This engine isn't a browser yet: try it with
`out/xwebview page.html`, which shows a local page.

| Works | Not yet |
|---|---|
| HTML5 parsing, including error recovery, templates, SVG/MathML trees | `<selectedcontent>` mirroring |
| CSS parsing, selectors (not `:has()`), cascade, `@media`, `@import`, `var()`, `calc()`, `::before`/`::after` | **`@font-face`**, animations, transforms, `@supports` properly |
| Block and inline layout, margin collapsing, inline-block, lists | **grid** (lays out as blocks) |
| **Floats, tables (auto and fixed), flexbox**, positioning | sticky, z-index stacking contexts, collapsed-border conflict resolution |
| Backgrounds (colours), borders, rounded corners, overflow clipping, text, images beside the page | background images, gradients, shadows, per-corner radii, dashed borders |
| Scrolling with the wheel | **network, following links, forms, JavaScript** |

## Layers

- `dom.h`, `html_parser.h`: the tree and the WHATWG parser. No dependencies.
- `css.h`: CSS Syntax 3 tokenizer, rules, selectors, media queries.
- `style.h`: matching, cascade, computed values.
- `layout.h`: boxes and paint operations, asking a `TextMeasurer` about text.
- `text_measurer.h`, `tools/xweb_view.cc`: the only parts that know about xrender and xkit.

Pages are untrusted input. Depth, selector matching, `var()` expansion and imports all
have limits, and nothing a page does can make the parsers fail.

## Tests

- `xweb html5lib`: the html5lib tree-construction suite (`tests/html5lib`, MIT). Gaps are
  listed in `tests/html5lib_known_failures.txt`; a listed test that starts passing fails.
- `xweb css`, `style`, `layout`, `html`: unit tests.
- `xweb fuzz`: mutated and pathological input through the whole pipeline. Worth running
  in an ASan build after changing the engine.
- `scripts/xweb-compare.py sdk/xweb/tests/pages/*.html`: every element's box against
  headless Chromium. Needs Chromium, so it isn't in the unit suite.
- `headless-e2e.py webview`: a page in a real window, checked pixel by pixel.
