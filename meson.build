# ── sdk/xweb — FreeBSD-X's own web engine ─────────────────────────────────────
# Included via subdir('sdk/xweb') from the root meson.build.
# The engine proper needs no graphics: parsing, style and layout are testable alone.

xweb_inc = include_directories('include')

libxweb = static_library('xweb',
  'src/dom.cc',
  'src/url.cc',
  'src/encoding.cc',
  'src/html_tokenizer.cc',
  'src/html_parser.cc',
  'src/css_parser.cc',
  'src/style.cc',
  'src/layout.cc',
  include_directories : xweb_inc,
)

xweb_dep = declare_dependency(link_with : libxweb, include_directories : xweb_inc)

# Prints a page's parsed document.
xweb_dump = executable('xweb_dump', 'tools/xweb_dump.cc', dependencies : [xweb_dep])

# Measuring with the fonts we draw with.
libxweb_text = static_library('xweb_text',
  'src/text_measurer.cc',
  dependencies : [xweb_dep, xrender_dep, freetype_dep, harfbuzz_dep, fontconfig_dep],
  include_directories : include_directories('../../include', '../xrender/include'),
)
xweb_text_dep = declare_dependency(link_with : libxweb_text, dependencies : [xweb_dep, xrender_dep])

# Prints every element's box; scripts/xweb-compare.py checks them against Chromium.
xweb_layout = executable('xweb_layout', 'tools/xweb_layout.cc',
  dependencies : [xweb_text_dep, freetype_dep, harfbuzz_dep, fontconfig_dep, libpng_dep, vulkan_dep],
  include_directories : include_directories('../../include', '../xrender/include'),
)

# The html5lib conformance suite, run against the parser (tests/html5lib, known gaps
# listed in tests/html5lib_known_failures.txt).
test('xweb html5lib',
  executable('xweb_html5lib_test', 'tests/html5lib_runner.cc', dependencies : [xweb_dep]),
  args : [meson.current_source_dir() / 'tests' / 'html5lib', meson.current_source_dir() / 'tests' / 'html5lib_known_failures.txt'],
  suite : 'unit',
)

foreach unit : [['css', 'css_parser_test.cc'], ['style', 'style_test.cc'], ['layout', 'layout_test.cc'], ['fuzz', 'fuzz_test.cc'], ['html', 'html_parser_test.cc'], ['url', 'url_test.cc'], ['encoding', 'encoding_test.cc']]
  test('xweb ' + unit[0],
    executable('xweb_' + unit[0] + '_test', 'tests/unit' / unit[1], dependencies : [xweb_dep, gtest_dep]),
    suite : 'unit',
  )
endforeach
