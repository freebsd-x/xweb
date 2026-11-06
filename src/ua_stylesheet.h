#pragma once

// What every page starts from (the rendering section of the HTML standard).

namespace fbx::xweb::detail {

inline constexpr const char* kUserAgentStyle = R"css(
html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing,
main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt,
menu, ol, ul, details, summary, fieldset, table, caption, tr, thead, tbody, tfoot, optgroup, marquee { display: block; }
head, link, meta, script, style, title, template, area, base, basefont, datalist, noembed, noframes, param, rp,
source, track, option, dialog:not([open]), [hidden] { display: none; }
li { display: list-item; }
td, th { display: inline-block; padding: 1px; vertical-align: top; }
th { font-weight: bold; text-align: center; }
table { border-spacing: 2px; }
caption { text-align: center; }
html { color: black; }
body { margin: 8px; }
address, cite, dfn, em, i, var { font-style: italic; }
b, strong { font-weight: bold; }
blockquote, figure { margin: 1em 40px; }
center { text-align: center; }
code, kbd, listing, plaintext, pre, samp, tt, xmp { font-family: monospace; }
dd { margin-left: 40px; }
dir, dl, listing, menu, ol, p, plaintext, pre, ul, xmp { margin-top: 1em; margin-bottom: 1em; }
dir dir, dir dl, dir menu, dir ol, dir ul, dl dir, dl dl, dl menu, dl ol, dl ul, menu dir, menu dl, menu menu, menu ol,
menu ul, ol dir, ol dl, ol menu, ol ol, ol ul, ul dir, ul dl, ul menu, ul ol, ul ul { margin-top: 0; margin-bottom: 0; }
dir, menu, ol, ul { padding-left: 40px; }
ol { list-style-type: decimal; }
ul { list-style-type: disc; }
ul ul, ol ul, menu ul, dir ul { list-style-type: circle; }
ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square; }
h1 { font-size: 2em; margin-top: 0.67em; margin-bottom: 0.67em; font-weight: bold; }
h2 { font-size: 1.5em; margin-top: 0.83em; margin-bottom: 0.83em; font-weight: bold; }
h3 { font-size: 1.17em; margin-top: 1em; margin-bottom: 1em; font-weight: bold; }
h4 { font-size: 1em; margin-top: 1.33em; margin-bottom: 1.33em; font-weight: bold; }
h5 { font-size: 0.83em; margin-top: 1.67em; margin-bottom: 1.67em; font-weight: bold; }
h6 { font-size: 0.67em; margin-top: 2.33em; margin-bottom: 2.33em; font-weight: bold; }
hr { border: 1px inset gray; margin: 0.5em auto; }
listing, plaintext, pre, xmp { white-space: pre; }
textarea { white-space: pre-wrap; }
small { font-size: smaller; }
big { font-size: larger; }
sub, sup { font-size: smaller; }
s, strike, del { text-decoration: line-through; }
u, ins { text-decoration: underline; }
a:link { color: #0000EE; text-decoration: underline; }
mark { background-color: yellow; color: black; }
nobr { white-space: nowrap; }
img, video, canvas, iframe, embed, object, svg { display: inline-block; }
input, textarea, select, button { display: inline-block; font-size: 13px; }
input, textarea, select { border: 2px inset gray; padding: 1px 2px; background-color: white; }
button { border: 2px outset gray; padding: 1px 6px; background-color: #efefef; color: black; }
fieldset { margin-left: 2px; margin-right: 2px; padding: 0.35em 0.75em 0.625em; border: 2px groove gray; }
input[type=hidden] { display: none; }
)css";

}  // namespace fbx::xweb::detail
