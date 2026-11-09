#pragma once

// What every page starts from (the rendering section of the HTML standard).

namespace fbx::xweb::detail {

inline constexpr const char* kUserAgentStyle = R"css(
html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing,
main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt,
menu, ol, ul, details, summary, fieldset, optgroup, marquee { display: block; }
table { display: table; border-spacing: 2px; border-collapse: separate; box-sizing: border-box; }
caption { display: table-caption; text-align: center; }
thead, tbody, tfoot { display: table-row-group; vertical-align: middle; }
tr { display: table-row; vertical-align: inherit; }
col, colgroup { display: table-column; }
td, th { display: table-cell; padding: 1px; vertical-align: inherit; }
th { font-weight: bold; text-align: center; }
head, link, meta, script, style, title, template, area, base, basefont, datalist, noembed, noframes, param, rp,
source, track, option, dialog:not([open]), [hidden] { display: none; }
slot { display: contents; }
li { display: list-item; }
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
small { font-size: smaller; }
big { font-size: larger; }
sub, sup { font-size: smaller; }
sub { vertical-align: sub; }
sup { vertical-align: super; }
img { vertical-align: baseline; }
s, strike, del { text-decoration: line-through; }
u, ins { text-decoration: underline; }
a:link { color: #0000EE; text-decoration: underline; }
mark { background-color: yellow; color: black; }
nobr { white-space: nowrap; }
img, video, canvas, iframe, embed, object, svg { display: inline-block; }
input, textarea, select, button { display: inline-block; font: 13.3333px sans-serif; line-height: normal; text-align: start; text-indent: 0; text-transform: none; white-space: normal; }
input { line-height: normal !important; }
textarea { white-space: pre-wrap; }
input { border: 2px inset gray; padding: 1px 2px; background-color: white; }
textarea { border: 1px solid gray; padding: 2px; background-color: white; vertical-align: text-bottom; }
select { border: 1px solid gray; padding: 0; background-color: white; box-sizing: border-box; }
input[type=checkbox], input[type=radio] { border: none; padding: 0; margin: 3px 3px 3px 4px; }
input[type=radio] { margin: 3px 3px 0 5px; }
input[type=submit], input[type=reset], input[type=button] { padding: 1px 6px; border: 2px outset gray; background-color: #efefef; color: black; box-sizing: border-box; text-align: center; }
button { border: 2px outset gray; padding: 1px 6px; background-color: #efefef; color: black; box-sizing: border-box; text-align: center; }
fieldset { margin-left: 2px; margin-right: 2px; padding: 0.35em 0.75em 0.625em; border: 2px groove gray; }
input[type=hidden] { display: none; }
)css";

// A page with no doctype: what old pages were written against.
inline constexpr const char* kQuirksStyle = R"css(
table { font-weight: initial; font-style: initial; font-size: initial; line-height: initial; white-space: initial; text-align: initial; }
)css";

}  // namespace fbx::xweb::detail
