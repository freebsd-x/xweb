// Our JavaScript engine: the language and its library, script in, value or error out.

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>

#include "xweb/js.h"

using namespace fbx::xweb::js;

namespace {

// The script's last value as text, or "throws: <error>".
std::string run(const std::string& src) {
    Runtime rt;
    rt.setStepBudget(5'000'000);
    const Runtime::Result r = rt.evaluate(src);
    if (!r.ok) return "throws: " + r.error;
    if (r.value.isString()) return utf8(r.value.asString());
    return utf8(rt.toStringValue(r.value));
}

}  // namespace

TEST(Js, Arithmetic) {
    EXPECT_EQ(run("1 + 2 * 3"), "7");
    EXPECT_EQ(run("0.1 + 0.2"), "0.30000000000000004");
    EXPECT_EQ(run("'5' * '2'"), "10");
    EXPECT_EQ(run("'5' + 2"), "52");
    EXPECT_EQ(run("7 % -3"), "1");
    EXPECT_EQ(run("2 ** 3 ** 2"), "512");
    EXPECT_EQ(run("-7 >> 1"), "-4");
    EXPECT_EQ(run("-1 >>> 28"), "15");
    EXPECT_EQ(run("1 / 0"), "Infinity");
    EXPECT_EQ(run("0 / 0"), "NaN");
    EXPECT_EQ(run("123456789012345680000"), "123456789012345680000");
    EXPECT_EQ(run("1e21"), "1e+21");
    EXPECT_EQ(run("0.000001"), "0.000001");
    EXPECT_EQ(run("1e-7"), "1e-7");
}

TEST(Js, Equality) {
    EXPECT_EQ(run("null == undefined"), "true");
    EXPECT_EQ(run("null === undefined"), "false");
    EXPECT_EQ(run("'1' == 1"), "true");
    EXPECT_EQ(run("NaN == NaN"), "false");
    EXPECT_EQ(run("[1] == 1"), "true");
    EXPECT_EQ(run("({}) == '[object Object]'"), "true");
    EXPECT_EQ(run("Object.is(-0, 0)"), "false");
    EXPECT_EQ(run("'b' > 'a' && 'B' < 'a'"), "true");
}

TEST(Js, ScopesAndClosures) {
    EXPECT_EQ(run("let fs = []; for (let i = 0; i < 3; i++) fs.push(() => i); fs.map(f => f()).join()"), "0,1,2");
    EXPECT_EQ(run("var fs = []; for (var i = 0; i < 3; i++) fs.push(() => i); fs.map(f => f()).join()"), "3,3,3");
    EXPECT_EQ(run("f(); function f() { return 1 }"), "1");
    EXPECT_EQ(run("x; let x = 1"), "throws: ReferenceError: can't use 'x' before it's declared");
    EXPECT_EQ(run("const c = 1; c = 2"), "throws: TypeError: assignment to constant 'c'");
    EXPECT_EQ(run("{ let a = 1; { let a = 2; } a }"), "1");
    EXPECT_EQ(run("var o = (function () { var n = 0; return { inc: () => ++n }; })(); o.inc(); o.inc()"), "2");
    EXPECT_EQ(run("typeof notDeclared"), "undefined");
    EXPECT_EQ(run("notDeclared"), "throws: ReferenceError: notDeclared is not defined");
    EXPECT_EQ(run("(function () { 'use strict'; undeclared = 1; })()"), "throws: ReferenceError: undeclared is not defined");
    EXPECT_EQ(run("(function () { sloppy = 5; })(); sloppy"), "5");
}

TEST(Js, Functions) {
    EXPECT_EQ(run("function f(a, b = a + 1, ...r) { return [a, b, r.length]; } f(1).join() + ';' + f(1, 5, 7, 8).join()"), "1,2,0;1,5,2");
    EXPECT_EQ(run("function f() { return arguments.length; } f(1, 2, 3)"), "3");
    EXPECT_EQ(run("(function () { return this; }).call(5) instanceof Number"), "true");
    EXPECT_EQ(run("(function () { 'use strict'; return this; }).call(5)"), "5");
    EXPECT_EQ(run("var o = { n: 2, f() { return [1, 2].map(x => x * this.n); } }; o.f().join()"), "2,4");
    EXPECT_EQ(run("function f(a, b) { return a + b; } f.bind(null, 2)(3) + f.apply(null, [4, 5]) + f.length"), "16");
    EXPECT_EQ(run("var f = function g() { return typeof g; }; f()"), "function");
    EXPECT_EQ(run("let named = () => 1; named.name"), "named");
    EXPECT_EQ(run("function r() { return r(); } r()"), "throws: RangeError: too much recursion");
}

TEST(Js, ObjectsAndDestructuring) {
    EXPECT_EQ(run("const {a, b: {c}, d = 4, ...rest} = {a: 1, b: {c: 2}, e: 5, f: 6}; [a, c, d, Object.keys(rest)].join()"), "1,2,4,e,f");
    EXPECT_EQ(run("let [x, , y = 3, ...z] = [1, 2, undefined, 4, 5]; [x, y, z.join('')].join()"), "1,3,45");
    EXPECT_EQ(run("let a = 1, b = 2; [a, b] = [b, a]; a * 10 + b"), "21");
    EXPECT_EQ(run("const k = 'dyn'; ({[k + 1]: 1, ['x']: 2}).dyn1"), "1");
    EXPECT_EQ(run("({ get v() { return 7; }, set v(x) { this.w = x; } }).v"), "7");
    EXPECT_EQ(run("const o = {}; Object.defineProperty(o, 'x', { value: 1 }); o.x = 2; o.x"), "1");
    EXPECT_EQ(run("'use strict'; const o = Object.freeze({x: 1}); o.x = 2"), "throws: TypeError: can't assign to read-only property 'x'");
    EXPECT_EQ(run("const p = { hi() { return 'p'; } }; const c = Object.create(p); c.hi() + Object.getPrototypeOf(c).hi()"), "pp");
    EXPECT_EQ(run("JSON.stringify(Object.entries({a: 1, 2: 'b', 1: 'c'}))"), "[[\"1\",\"c\"],[\"2\",\"b\"],[\"a\",1]]");
    EXPECT_EQ(run("null.x"), "throws: TypeError: can't read property 'x' of null");
    EXPECT_EQ(run("const o = {a: {b: null}}; [o?.a?.b?.c, o.x?.y.z, o.a?.['b']].join('|')"), "||");
    EXPECT_EQ(run("const {x} = null"), "throws: TypeError: can't destructure null");
}

TEST(Js, Classes) {
    EXPECT_EQ(run(R"JS(
        class A { constructor(x) { this.x = x; } get double() { return this.x * 2; } static of(x) { return new A(x); } }
        class B extends A { y = 10; constructor() { super(3); } get double() { return super.double + this.y; } }
        const b = new B(); [b.double, A.of(4).double, b instanceof A, Object.getPrototypeOf(B) === A].join())JS"),
              "16,8,true,true");
    EXPECT_EQ(run("class C {} C()"), "throws: TypeError: a class constructor must be called with new");
    EXPECT_EQ(run("class D extends Object { constructor() { this.x = 1; } } new D()"), "throws: ReferenceError: super() must be called before using this");
    EXPECT_EQ(run("class E extends Error { constructor(m) { super(m); this.name = 'E'; } } String(new E('boom'))"), "E: boom");
    EXPECT_EQ(run("class F { static count = 3; } F.count"), "3");
}

TEST(Js, ControlFlow) {
    EXPECT_EQ(run("let s = ''; outer: for (const i of [1, 2, 3]) { for (const j of [1, 2]) { if (j == 2) continue outer; if (i == 3) break outer; s += i; } } s"), "12");
    EXPECT_EQ(run("let s = ''; switch (2) { case 1: s += 'a'; case 2: s += 'b'; case 3: s += 'c'; break; default: s += 'd'; } s"), "bc");
    EXPECT_EQ(run("function f() { try { return 'try'; } finally { log = 'finally'; } } var log; f() + log"), "tryfinally");
    EXPECT_EQ(run("function f() { try { throw 1; } catch (e) { return e + 1; } } f()"), "2");
    EXPECT_EQ(run("let n = 0; do { n++; } while (n < 5); n"), "5");
    EXPECT_EQ(run("let s = ''; for (const k in {a: 1, b: 2}) s += k; s"), "ab");
    EXPECT_EQ(run("let s = ''; for (const c of 'h😀') s += '[' + c + ']'; s"), "[h][😀]");
    EXPECT_EQ(run("throw new RangeError('nope')"), "throws: RangeError: nope");
    EXPECT_EQ(run("throw 'plain'"), "throws: Uncaught plain");
}

TEST(Js, Library) {
    EXPECT_EQ(run("[3, 1, 10, 2].sort().join()"), "1,10,2,3");
    EXPECT_EQ(run("[3, 1, 10, 2].sort((a, b) => a - b).join()"), "1,2,3,10");
    EXPECT_EQ(run("[1, 2, 3, 4].filter(x => x % 2).map(x => x * 10).reduce((a, b) => a + b)"), "40");
    EXPECT_EQ(run("[1, [2, [3, [4]]]].flat(2).length"), "4");
    EXPECT_EQ(run("Array.from({length: 3}, (_, i) => i).concat([9]).slice(-2).join()"), "2,9");
    EXPECT_EQ(run("const a = [1, 2, 3, 4, 5]; a.splice(1, 2, 'x'); a.join()"), "1,x,4,5");
    EXPECT_EQ(run("[NaN].includes(NaN) + ',' + [NaN].indexOf(NaN)"), "true,-1");
    EXPECT_EQ(run("' padded '.trim().padStart(8, '*').toUpperCase()"), "**PADDED");
    EXPECT_EQ(run("'a-b-c'.split('-', 2).join() + 'xyz'.replace('y', '[$&]') + 'aaa'.replaceAll('a', 'b')"), "a,bx[y]zbbb");
    EXPECT_EQ(run("String.fromCodePoint(0x1F600).length + 'abc'.at(-1) + 'abc'.codePointAt(1)"), "2c98");
    EXPECT_EQ(run("[parseInt('08'), parseInt('ff', 16), parseFloat('.5e1x'), Number(''), Number('0b101'), Number('12px')].join()"), "8,255,5,0,5,NaN");
    EXPECT_EQ(run("[(255).toString(2), (0.5).toString(16), (1234.5678).toFixed(2), (0.00001).toPrecision(2), (12345).toLocaleString()].join(' ')"), "11111111 0.8 1234.57 0.000010 12,345");
    EXPECT_EQ(run("[Math.round(2.5), Math.round(-2.5), Math.max(), Math.min(1, -0) === 0, Math.trunc(-4.7), Math.sign(-3)].join()"), "3,-2,-Infinity,true,-4,-1");
    EXPECT_EQ(run("JSON.stringify({a: [1, 'two', null, undefined, () => 1], b: undefined, c: {d: true}}, null, 1)"),
              "{\n \"a\": [\n  1,\n  \"two\",\n  null,\n  null,\n  null\n ],\n \"c\": {\n  \"d\": true\n }\n}");
    EXPECT_EQ(run("JSON.parse('{\"a\":[1,2,{\"b\":\"\\\\u00e9\"}]}', (k, v) => typeof v === 'number' ? v * 10 : v).a.join()"), "10,20,[object Object]");
    EXPECT_EQ(run("JSON.parse('{bad}')"), "throws: SyntaxError: JSON.parse: unexpected input at position 1");
    EXPECT_EQ(run("const m = new Map([[1, 'a']]); m.set(NaN, 'n').set(1, 'b'); [m.size, m.get(1), m.get(NaN), [...m.keys()].length].join()"), "2,b,n,2");
    EXPECT_EQ(run("const s = new Set('hello'); s.delete('l'); [...s].join('')"), "heo");
    EXPECT_EQ(run("encodeURIComponent('a b/ö') + decodeURI('%41%2F')"), "a%20b%2F%C3%B6A%2F");
    EXPECT_EQ(run("const s = Symbol('d'); const o = {[s]: 1}; [o[s], s.description, typeof s, Object.keys(o).length].join()"), "1,d,symbol,0");
    EXPECT_EQ(run("const t = (s, ...v) => s.raw.join('|') + v.join(); t`a${1}b${2}`"), "a|b|1,2");
    EXPECT_EQ(run("Object.prototype.toString.call([]) + Object.prototype.toString.call(null)"), "[object Array][object Null]");
}

TEST(Js, SyntaxErrors) {
    EXPECT_EQ(run("let let = 1"), "throws: SyntaxError: let can't be a name here (line 1)");
    EXPECT_EQ(run("1 +"), "throws: SyntaxError: unexpected end of script (line 1)");
    EXPECT_EQ(run("a ?? b || c"), "throws: SyntaxError: ?? can't be mixed with && or || without parentheses (line 1)");
    EXPECT_EQ(run("'unterminated"), "throws: SyntaxError: unterminated string (line 1)");
    EXPECT_EQ(run("var x = 1\nvar y = 2\nx + y"), "3");   // semicolons inserted
    EXPECT_EQ(run("function f() { return\n1 } f()"), "undefined");
}

TEST(Js, Limits) {
    // A loop that never ends is stopped; so is too much memory or nesting.
    // Where it was stopped, to know which script.
    EXPECT_EQ(run("while (true) {}"), "throws: Stopped: the script ran too long (script:1:14)");
    EXPECT_EQ(run("for (;;) { try { } catch (e) { } }"), "throws: Stopped: the script ran too long (script:1:10)");
    std::string deep(5000, '(');
    deep += "1" + std::string(5000, ')');
    EXPECT_EQ(run(deep), "throws: SyntaxError: too deeply nested (line 1, column 800)");
    EXPECT_EQ(run("let s = 'x'; while (true) s += s"), "throws: RangeError: string too long");
    EXPECT_EQ(run("new Array(1e9)"), "throws: RangeError: array too long");
    EXPECT_EQ(run("const a = []; a.length = 4294967296"), "throws: RangeError: invalid array length");
}

TEST(Js, GarbageCollection) {
    Runtime rt;
    ASSERT_TRUE(rt.evaluate("var keep = {n: 1}; for (let i = 0; i < 10000; i++) { ({a: [i]}); }").ok);
    const size_t before = rt.heapSize();
    rt.collectGarbage();
    EXPECT_LT(rt.heapSize(), before - 5000);
    const Runtime::Result r = rt.evaluate("keep.n + 1");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(rt.toNumber(r.value), 2);
}

TEST(Js, Console) {
    Runtime rt;
    std::string got;
    rt.setConsole([&](std::string_view level, const std::string& text) { got += std::string(level) + ":" + text + ";"; });
    ASSERT_TRUE(rt.evaluate("console.log('a', 1, {x: [2]}); console.error(new TypeError('t'))").ok);
    EXPECT_EQ(got, "log:a 1 {\"x\":[2]};error:TypeError: t (script:1:59);");
}

TEST(Js, Fuzz) {
    // Mangled scripts never crash or hang: they throw, or stop at the budget.
    const std::string corpus[] = {
        "function f(a,b=1,...c){return [a,b,c]}f(1,2,3)", "class A extends (class{}){#p=1;static s;get x(){return 1}}new A().x",
        "for(let i=0;i<3;i++)for(const k in {a:1})for(const v of [1,2]);", "({a,b:[c,...d]={},...e}=x=>x?.y??`t${1}`)",
        "label:{try{throw 1}catch({e}){}finally{break label}}", "var o={get[1](){},set a(v){},async*g(){},m(){}};o",
        "switch(1){case 1:default:let q;}", "a=>b=>c=>d", "new new X()()", "x = /re/g.test(y)", "'\\u{10FFFF}\\x41\\101'",
    };
    std::mt19937 rng(5);
    for (int round = 0; round < 3000; ++round) {
        std::string s = corpus[rng() % std::size(corpus)];
        const int edits = 1 + rng() % 6;
        for (int e = 0; e < edits && !s.empty(); ++e) {
            const size_t at = rng() % s.size();
            switch (rng() % 4) {
                case 0: s.erase(at, 1 + rng() % 3); break;
                case 1: s.insert(at, 1, "(){}[]`'\"/\;,.=>?:#*-+!~"[rng() % 23]); break;
                case 2: s[at] = static_cast<char>(rng() % 256); break;
                default: s.insert(at, s.substr(rng() % s.size(), rng() % 8)); break;
            }
        }
        (void)run(s);
    }
    // Pathological shapes.
    (void)run(std::string(100000, '['));
    (void)run(std::string(100000, '{'));
    (void)run("x" + std::string(50000, '.') );
    (void)run(std::string(20000, '!') + "1");
    (void)run("`" + std::string(20000, '$') + "{");
    (void)run("a" + std::string(30000, '?') );
    std::string chain = "0";
    for (int i = 0; i < 10000; ++i) chain += "+0";
    EXPECT_EQ(run(chain), "0");
}

TEST(Js, RegExpMatching) {
    EXPECT_EQ(run("'2024-11-08'.match(/(\\d+)-(\\d+)-(\\d+)/).slice(1).join()"), "2024,11,08");
    EXPECT_EQ(run("/(?<y>\\d{4})-(?<m>\\d\\d)/.exec('on 2020-05').groups.m"), "05");
    EXPECT_EQ(run("[.../(a)|(b)/.exec('b')].map(String).join()"), "b,undefined,b");
    // Captures inside a repeat are cleared each time round; an empty round ends the loop.
    EXPECT_EQ(run("/(z)((a+)?(b+)?(c))*/.exec('zaacbbbcac').map(String).join()"), "zaacbbbcac,z,ac,a,undefined,c");
    EXPECT_EQ(run("/(a*)*b/.test('aaac')"), "false");
    EXPECT_EQ(run("[/<(.*?)>/.exec('<a><b>')[1], /<(.*)>/.exec('<a><b>')[1], /x{2,3}?/.exec('xxxx')[0]].join()"), "a,a><b,xx");
    EXPECT_EQ(run("[/(?<=\\$)\\d+/.exec('cost $42')[0], /(?<!\\$)\\b\\d+/.exec('$42 or 17')[0], /\\d+(?!%)/.exec('50%')[0]].join()"), "42,17,5");
    EXPECT_EQ(run("/(?=(a+))a*b\\1/.exec('baaabac')[0]"), "aba");
    EXPECT_EQ(run("/^(\\w+)\\s\\1$/.test('hello hello')"), "true");
    EXPECT_EQ(run("[/^abc$/m.test('x\\nabc'), /a.c/s.test('a\\nc'), /a.c/.test('a\\nc'), /σ/i.test('Σ'), /é/i.test('É')].join()"),
              "true,true,false,true,true");
    EXPECT_EQ(run("[/^.$/u.test('😀'), /^.$/.test('😀'), /\\u{1F600}/u.test('😀'), /\\p{L}+/u.exec('12 héllo')[0]].join()"),
              "true,false,true,héllo");
    // The web's leniency outside unicode mode.
    EXPECT_EQ(run("[/a{/.test('a{'), /]/.test(']'), /\\101/.test('A'), /\\8/.test('8'), /[\\w-.]+/.exec('a.b-c d')[0], /\\k/.test('k')].join()"),
              "true,true,true,true,a.b-c,true");
    EXPECT_EQ(run("const g = /o/g; [g.test('foo'), g.lastIndex, g.test('foo'), g.lastIndex, g.test('foo'), g.lastIndex].join()"),
              "true,2,true,3,false,0");
    EXPECT_EQ(run("const y = /o/y; [y.test('foo'), (y.lastIndex = 1, y.test('foo'))].join()"), "false,true");
}

TEST(Js, RegExpStrings) {
    EXPECT_EQ(run("'a1b2c3'.replace(/\\d/g, d => d * 2)"), "a2b4c6");
    EXPECT_EQ(run("'John Smith'.replace(/(\\w+)\\s(\\w+)/, '$2, $1')"), "Smith, John");
    EXPECT_EQ(run("'2020-05'.replace(/(?<y>\\d{4})-(?<m>\\d\\d)/, '$<m>/$<y>')"), "05/2020");
    EXPECT_EQ(run("'abc'.replace(/b/, \"[$`|$&|$']\")"), "a[a|b|c]c");
    EXPECT_EQ(run("[...'a1b22c333'.matchAll(/\\d+/g)].map(m => m[0] + '@' + m.index).join()"), "1@1,22@3,333@6");
    EXPECT_EQ(run("JSON.stringify(['a, b,c'.split(/\\s*,\\s*/), 'abc'.split(/(b)/), ''.split(/x/), 'test'.split(/(?:)/, 2)])"),
              R"([["a","b","c"],["a","b","c"],[""],["t","e"]])");
    EXPECT_EQ(run("['aXbX'.match(/x/gi).join(), 'abc'.search(/c/), 'abc'.match(/z/), 'aaa'.replaceAll(/a/g, 'b')].join('|')"), "X,X|2||bbb");
    EXPECT_EQ(run("/(\\d+)/.exec('ab 42'); RegExp.$1"), "42");
    EXPECT_EQ(run("[String(/a\\/b/g), new RegExp('a/b').source, String(new RegExp('')), new RegExp(/ab/g, 'i').flags].join()"),
              "/a\\/b/g,a\\/b,/(?:)/,i");
    EXPECT_EQ(run("Object.prototype.toString.call(/x/)"), "[object RegExp]");
    EXPECT_EQ(run("class R extends RegExp {} const r = new R('x', 'g'); [r instanceof R, r.test('x'), r.flags].join()"), "true,true,g");
    // A page's own matcher object, through the Symbol methods.
    EXPECT_EQ(run("'abc'.replace({ [Symbol.replace](s, w) { return s + w; } }, '!')"), "abc!");
}

TEST(Js, RegExpErrorsAndLimits) {
    EXPECT_EQ(run("/(/"), "throws: SyntaxError: invalid regular expression: unterminated group (line 1)");
    EXPECT_EQ(run("new RegExp('a', 'gg')"), "throws: SyntaxError: Invalid regular expression flags 'gg'");
    EXPECT_EQ(run("try { new RegExp('[b-a]') } catch (e) { e.name }"), "SyntaxError");
    EXPECT_EQ(run("try { /\\p{L}/u.test('x') && new RegExp('\\\\p{', 'u') } catch (e) { e.name }"), "SyntaxError");
    EXPECT_EQ(run("try { 'x'.matchAll(/x/) } catch (e) { e.name }"), "TypeError");
    // Catastrophic backtracking stops at the step budget, not never.
    EXPECT_NE(run("/(a+)+b/.test('a'.repeat(40))").find("ran too long"), std::string::npos);
    // A long text is fine: simple repeats don't stack a frame per character.
    EXPECT_EQ(run("const s = 'x'.repeat(1 << 20) + 'y'; [/x*y/.exec(s)[0].length, /(?:x|z)*?y/.test(s), s.split(/x/).length].join()"),
              "1048577,true,1048577");
    EXPECT_EQ(run("var a = ['x']; a.foo = 1; a.index = 2; [a.foo, a.index, a.length].join()"), "1,2,1");
}

TEST(Js, RegExpFuzz) {
    // Mangled patterns compile or throw a SyntaxError, and every one runs to an answer.
    const std::string corpus[] = {
        "(?<y>\\d{4})-(?<m>\\d\\d)", "^(?:a|b)*?c$", "(?<=\\$)\\d+(?!%)", "[\\w-.]+@[^\\s]{2,}", "(a*)*b\\1\\k<n>(?<n>x)",
        "\\p{Lu}\\P{N}[\\u{1F600}-\\u{1F64F}]", "a{2,3}?b+?c*?[^]$", "\\cJ\\x41\\u0042\\101\\0[\\b]",
    };
    const char* flags[] = {"", "g", "i", "u", "giu", "msy", "dv"};
    std::mt19937 rng(61);
    for (int round = 0; round < 3000; ++round) {
        std::string p = corpus[rng() % std::size(corpus)];
        const int edits = 1 + rng() % 5;
        for (int e = 0; e < edits && !p.empty(); ++e) {
            const size_t at = rng() % p.size();
            switch (rng() % 4) {
                case 0: p.erase(at, 1 + rng() % 3); break;
                case 1: p.insert(at, 1, "()[]{}?*+|^$\\.-<>=!:,"[rng() % 21]); break;
                case 2: p[at] = static_cast<char>(32 + rng() % 95); break;
                default: p.insert(at, p.substr(rng() % p.size(), rng() % 6)); break;
            }
        }
        std::string escaped;
        for (char c : p) {
            if (c == '\\' || c == '\'') escaped += '\\';
            escaped += c;
        }
        const std::string r = run("try { const re = new RegExp('" + escaped + "', '" + flags[rng() % std::size(flags)] +
                                  "'); [re.exec('aab$42 Hé 😀 2020-05 x@yy.zz'), 'aXb'.replace(re, '$1'), 'a,b'.split(re)].length } "
                                  "catch (e) { e.name }");
        EXPECT_TRUE(r == "3" || r == "SyntaxError" || r.find("ran too long") != std::string::npos) << p << " -> " << r;
    }
    // Pathological shapes.
    EXPECT_EQ(run("try { new RegExp('('.repeat(100000)) } catch (e) { e.name }"), "SyntaxError");
    EXPECT_EQ(run("new RegExp('(?:a{1000}){1000}').test('a'.repeat(1000000))"), "true");   // single-character repeats stay small
    EXPECT_EQ(run("try { new RegExp('(?:(?:a|b){1000}){1000}') } catch (e) { e.message }"),
              "Invalid regular expression: /(?:(?:a|b){1000}){1000}/: regular expression too large");
    EXPECT_EQ(run("new RegExp('(?:'.repeat(400) + 'x' + ')'.repeat(400)).test('x')"), "true");
}

namespace {

// Runs a script, then its promise jobs; what it logged, one line each.
std::string runAsync(const std::string& src) {
    Runtime rt;
    rt.setStepBudget(5'000'000);
    std::string out;
    rt.setConsole([&](std::string_view, const std::string& text) { out += text + "\n"; });
    rt.setErrorReporter([&](const std::string& message) { out += "! " + message + "\n"; });
    const Runtime::Result r = rt.evaluate(src);
    if (!r.ok) out += "throws: " + r.error + "\n";
    rt.runJobs();
    return out;
}

}  // namespace

TEST(Js, Generators) {
    EXPECT_EQ(run("function* g() { const x = yield 1; yield x * 2; return 'end'; } const it = g();"
                  "[it.next().value, it.next(5).value, it.next().value, it.next().done].join()"), "1,10,end,true");
    EXPECT_EQ(run("function* nat() { let i = 0; while (true) yield i++; } const out = []; for (const n of nat()) { if (n > 3) break; out.push(n); } out.join()"), "0,1,2,3");
    EXPECT_EQ(run("let fin = 0; function* g() { try { yield 1; yield 2; } finally { fin++; } } for (const v of g()) break; fin"), "1");
    EXPECT_EQ(run("function* g() { try { yield 1; } catch (e) { yield 'caught ' + e; } } const it = g(); it.next(); it.throw('x').value"), "caught x");
    EXPECT_EQ(run("function* g() { try { yield 1; } finally { yield 'cleanup'; } } const it = g(); it.next(); [it.return(7).value, it.next().value].join()"), "cleanup,7");
    EXPECT_EQ(run("function* inner() { const r = yield 'a'; return r + '!'; } function* outer() { const v = yield* inner(); yield v; }"
                  "const it = outer(); [it.next().value, it.next('b').value].join()"), "a,b!");
    EXPECT_EQ(run("const [a, , b, ...rest] = (function* () { yield 1; yield 2; yield 3; yield 4; yield 5; })(); [a, b, rest].join()"), "1,3,4,5");
    EXPECT_EQ(run("function* g() { yield 1; } const it = g(); Object.prototype.toString.call(it) + ' ' + (it[Symbol.iterator]() === it)"), "[object Generator] true");
    EXPECT_EQ(run("function* g() { yield g2.next(); } const g2 = g(); g2.next()"), "throws: TypeError: the generator is already running");
    EXPECT_EQ(run("class C { *[Symbol.iterator]() { yield 'x'; yield 'y'; } } [...new C()].join('')"), "xy");
}

TEST(Js, AsyncFunctionsAndPromises) {
    EXPECT_EQ(runAsync("async function f(x) { console.log('start'); const y = await x; console.log('got ' + y); return y * 2; }"
                       "f(Promise.resolve(21)).then(v => console.log('result ' + v)); console.log('sync end');"),
              "start\nsync end\ngot 21\nresult 42\n");
    // The order of jobs, as the specification has it.
    EXPECT_EQ(runAsync("const o = []; Promise.resolve().then(() => o.push(1)).then(() => o.push(3)); Promise.resolve().then(() => o.push(2));"
                       "(async () => { o.push('a'); await null; o.push('b'); })();"
                       "Promise.resolve().then(() => Promise.resolve().then(() => console.log(o.join(''))));"),
              "a12b3\n");
    EXPECT_EQ(runAsync("(async () => { try { await Promise.reject(new TypeError('boom')); } catch (e) { console.log('caught ' + e.message); } finally { console.log('finally'); } })()"),
              "caught boom\nfinally\n");
    EXPECT_EQ(runAsync("Promise.all([1, Promise.resolve(2), new Promise(r => r(3))]).then(v => console.log(v.join()));"
                       "Promise.allSettled([Promise.reject(1), 2]).then(v => console.log(v.map(e => e.status).join()));"
                       "Promise.any([Promise.reject(1), Promise.reject(2)]).catch(e => console.log(e.name + ' ' + e.errors.join()));"
                       "Promise.race([new Promise(() => {}), Promise.resolve('fast')]).then(v => console.log(v));"),
              "1,2,3\nrejected,fulfilled\nAggregateError 1,2\nfast\n");   // race settles a tick later, as the spec has it
    EXPECT_EQ(runAsync("const {promise, resolve} = Promise.withResolvers(); promise.then(v => console.log(v)); resolve('later');"
                       "Promise.resolve(1).finally(() => console.log('fin')).then(v => console.log('kept ' + v));"),
              "later\nfin\nkept 1\n");
    EXPECT_EQ(runAsync("const thenable = { then(r) { r('from thenable'); } }; (async () => console.log(await thenable))();"),
              "from thenable\n");
    EXPECT_EQ(runAsync("new Promise((_, reject) => reject(new Error('nobody listens')));"),
              "! Uncaught (in promise) Error: nobody listens (script:1:44)\n");
    EXPECT_EQ(runAsync("async function* g() { yield 1; await null; yield 2; } (async () => { const o = []; for await (const v of g()) o.push(v);"
                       "for await (const v of [Promise.resolve('p'), 'q']) o.push(v); console.log(o.join()); })()"),
              "1,2,p,q\n");
    EXPECT_EQ(runAsync("class P extends Promise {} const p = P.resolve(1).then(x => x); console.log(p instanceof P, Object.prototype.toString.call(p))"),
              "true [object Promise]\n");
    EXPECT_EQ(runAsync("async function r(n) { return n === 0 ? 0 : 1 + await r(n - 1); } r(200).then(v => console.log(v))"), "200\n");
}

TEST(Js, Modules) {
    Runtime rt;
    std::map<std::string, std::string> files = {
        {"https://m.test/lib/a.js", "export let count = 0; export function inc() { count++; } export default function () { return 'def'; }"},
        {"https://m.test/lib/b.js", "export { count as total } from './a.js'; export * from './c.js';"},
        {"https://m.test/lib/c.js", "export const cc = 'C'; await null;"},
        {"https://m.test/lib/cyc.js", "import { main } from '../main.js'; export const fromCycle = () => typeof main;"},
        {"https://m.test/dyn.js", "export const x = 42;"},
    };
    int fetches = 0;
    rt.setModuleLoader([&](const std::string& url, std::function<void(ModuleSource)> done) {
        ++fetches;
        const auto it = files.find(url);
        done(it == files.end() ? ModuleSource{false, {}, "not found"} : ModuleSource{true, it->second, {}});
    });
    std::string out;
    rt.setConsole([&](std::string_view, const std::string& text) { out += text + "\n"; });
    bool finished = false;
    rt.evaluateModule(R"(
        import def, { count, inc } from './lib/a.js';
        import * as ns from './lib/a.js';
        import { total, cc } from './lib/b.js';
        import { fromCycle } from './lib/cyc.js';
        export function main() {}
        inc();
        console.log(def(), count, ns.count, total, cc, fromCycle(), typeof this, Object.keys(ns).join());
        try { count = 9; } catch (e) { console.log(e.name); }
        const m = await import('./dyn.js');
        console.log('dynamic', m.x, import.meta.url);
    )", "https://m.test/main.js", [&](Runtime::Result r) {
        finished = true;
        if (!r.ok) out += "failed: " + r.error + "\n";
    });
    rt.runJobs();
    EXPECT_TRUE(finished);
    EXPECT_EQ(out, "def 1 1 1 C function undefined count,default,inc\nTypeError\ndynamic 42 https://m.test/main.js\n");
    EXPECT_EQ(fetches, 5);   // each module once, however often it's imported

    std::string error;
    rt.evaluateModule("import { nope } from './lib/c.js';", "https://m.test/bad.js", [&](Runtime::Result r) { error = r.error; });
    rt.runJobs();
    EXPECT_EQ(error, "SyntaxError: https://m.test/lib/c.js doesn't export 'nope' (imported by https://m.test/bad.js)");
    rt.evaluateModule("import x from 'bare-name';", "https://m.test/bare.js", [&](Runtime::Result r) { error = r.error; });
    rt.runJobs();
    EXPECT_NE(error.find("can't resolve the module specifier 'bare-name'"), std::string::npos) << error;

    // An import map names it.
    ASSERT_TRUE(rt.import_map.add(R"({"imports": {"bare-name": "/lib/c.js", "lib/": "/lib/"}})", "https://m.test/"));
    EXPECT_EQ(rt.import_map.resolve("bare-name", "https://m.test/x.js"), "https://m.test/lib/c.js");
    EXPECT_EQ(rt.import_map.resolve("lib/a.js", "https://m.test/x.js"), "https://m.test/lib/a.js");
    EXPECT_EQ(rt.import_map.resolve("../up.js", "https://m.test/d/x.js"), "https://m.test/up.js");
    out.clear();
    rt.evaluateModule("import { cc } from 'bare-name'; console.log('mapped', cc);", "https://m.test/mapped.js");
    rt.runJobs();
    EXPECT_EQ(out, "mapped C\n");
    EXPECT_EQ(Runtime::resolveSpecifier("./a/../b.js?q#f", "https://h.test/x/y.js"), "https://h.test/x/b.js?q");
    EXPECT_EQ(moduleRequests("import a from './a.js'; export * from \"./b.js\"; import './a.js';"), (std::vector<std::string>{"./a.js", "./b.js"}));
}

TEST(Js, CoroutinesAndTheCollector) {
    Runtime rt;
    rt.setStepBudget(50'000'000);
    std::string out;
    rt.setConsole([&](std::string_view, const std::string& text) { out += text + "\n"; });
    // Values only a suspended frame holds: a member's base, call arguments, array
    // elements, a for-of iterator, a binary operand.
    ASSERT_TRUE(rt.evaluate(R"(
        var gates = [];
        const gate = () => new Promise(r => gates.push(r));
        (async () => {
            const a = ({id: 'base'})[await gate()];
            const b = ((x, y, z) => x.v + y + z.v)({v: 1}, await gate(), {v: 100});
            const c = [{n: 1}, await gate(), {n: 3}].map(o => o.n).join('');
            const o = {same: true};
            const d = o === await gate();
            let e = '';
            for (const v of [{k: 'p'}, {k: 'q'}]) { await gate(); e += v.k; }
            console.log(a, b, c, d, e);
        })();
        var it = (function* () { const keep = {deep: {deeper: 'kept'}}; yield 1; yield keep.deep.deeper; })();
        it.next();
        var dropped = (function* () { try { yield 1; } finally { console.log('WRONG: ran at collection'); } })();
        dropped.next();
        dropped = null;
        for (let i = 0; i < 100; i++) (async () => { const x = {i}; await new Promise(() => {}); return x; })();
    )").ok);
    const auto churn = [&] {
        ASSERT_TRUE(rt.evaluate("for (let i = 0; i < 20000; i++) ({a: [i], s: 'x' + i});").ok);
        rt.collectGarbage();
    };
    const size_t settled = rt.heapSize();
    churn();
    for (const char* v : {"'id'", "10", "({n: 2})", "undefined", "0", "0"}) {
        churn();
        const std::string resolve = std::string("gates.shift()(") + v + ")";
        ASSERT_TRUE(rt.evaluate(resolve).ok) << resolve;
        rt.runJobs();
    }
    churn();
    const Runtime::Result r = rt.evaluate("it.next().value");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(utf8(r.value.asString()), "kept");
    EXPECT_EQ(out, "base 111 123 false pq\n");
    // The dropped generator and the forever-waiting async calls were collected.
    EXPECT_LT(rt.heapSize(), settled + 2000);
}

TEST(Js, WeakCollectionsAndMapIndex) {
    EXPECT_EQ(run("const wm = new WeakMap(); const k = {}; wm.set(k, 1); [wm.get(k), wm.has({}), wm.delete(k), wm.has(k)].join()"), "1,false,true,false");
    EXPECT_EQ(run("try { new WeakMap().set(1, 1) } catch (e) { e.name }"), "TypeError");
    EXPECT_EQ(run("const m = new Map(); for (let i = 0; i < 100000; i++) m.set('k' + i, i); for (let i = 0; i < 99990; i++) m.delete('k' + i); [m.size, m.get('k99999'), [...m.keys()].length].join()"), "10,99999,10");
    EXPECT_EQ(run("const s = new Set([0, -0, NaN, NaN, '1', 1]); [s.size, s.has(-0)].join()"), "4,true");
    Runtime rt;
    ASSERT_TRUE(rt.evaluate("var wm = new WeakMap(); var keep = {}; wm.set(keep, {v: 'kept'}); for (let i = 0; i < 1000; i++) wm.set({}, {big: i});"
                            "var chainA = {}; var chainB = {}; wm.set(chainA, chainB); wm.set(chainB, 'through a'); chainB = null;"
                            "var ref = new WeakRef({}); var held = new WeakRef(keep);").ok);
    const size_t before = rt.heapSize();
    rt.collectGarbage();
    EXPECT_LT(rt.heapSize(), before - 1500);
    const Runtime::Result r = rt.evaluate("[wm.get(keep).v, wm.get(wm.get(chainA)), ref.deref(), held.deref() === keep].join()");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(utf8(r.value.asString()), "kept,through a,,true");
}

TEST(Js, EarlyErrors) {
    for (const char* bad : {"function* g() { var yield; }", "async function f() { var await; }", "function f(a, a) { 'use strict'; }",
                            "(a, a) => 1", "function f(a = 1) { 'use strict'; }", "a: a: ;", "break;", "while (1) { continue x; }",
                            "function f() { super.x; }", "new.target", "let x; var x;", "for (;;) function f() {}",
                            "function* g(x = yield) {}", "async function f(x = await 1) {}", "[...a, ] = []", "'use strict'; let = 1"}) {
        const std::string r = run(bad);
        EXPECT_EQ(r.rfind("throws: SyntaxError", 0), 0u) << bad << " → " << r;
    }
    for (const char* good : {"try {} catch { let t; } try { var t; } catch {}", "{ let r; } { var r; }", "function f(a) { var a; return a; } f(1)",
                             "var yield = 1; yield", "function f() { { function g() {} } let h; }", "a: { break a; }",
                             "x: for (;;) { for (;;) { continue x; } }", "class A { m() { return () => super.toString; } }", "(function () { new.target })"}) {
        const std::string r = run(good);
        EXPECT_NE(r.rfind("throws: SyntaxError", 0), 0u) << good << " → " << r;
    }
}

TEST(Js, CoroutineFuzz) {
    // Random mixes of generators, async functions, throws and returns: never a crash.
    std::mt19937 rng(62);
    const char* parts[] = {"yield 1;", "yield* [1, 2];", "await null;", "throw new Error('x');", "return 3;", "try { yield 2; } finally { await null; }",
                           "for (const v of g()) { if (v) break; }", "await Promise.reject(1).catch(() => 0);", "[await 1, yield 4];",
                           "f(); g().next();", "for await (const v of ag()) break;", "while (true) yield;"};
    for (int round = 0; round < 60; ++round) {
        std::string body;
        for (int i = 0; i < 6; ++i) body += parts[rng() % std::size(parts)];
        const std::string src = "async function* ag() { " + body + " }\nfunction* g() { yield 1; }\nasync function f() { try { const it = ag(); "
                                "for (let i = 0; i < 5; i++) await it.next(); await it.return(9); } catch (e) {} }\nf(); f();";
        Runtime rt;
        rt.setStepBudget(400'000);
        rt.evaluate(src);
        try {
            rt.runJobs();
        } catch (const Terminated&) {
        }
        rt.collectGarbage();
    }
}

TEST(Js, Date) {
    EXPECT_EQ(run("new Date(Date.UTC(2026, 10, 8, 4, 5, 6, 789)).toISOString()"), "2026-11-08T04:05:06.789Z");
    EXPECT_EQ(run("Date.parse('2026-11-08T04:05:06.789+09:00')"), "1794078306789");
    EXPECT_EQ(run("Date.parse('2026-11-08')"), "1794096000000");
    EXPECT_EQ(run("const d = new Date(1794110706789); Date.parse(d.toUTCString()) === 1794110706000 && Date.parse(String(d)) === 1794110706000"), "true");
    EXPECT_EQ(run("new Date(2024, 1, 29, 12).getDate() + ',' + new Date(2024, 0, 31).getMonth()"), "29,0");
    EXPECT_EQ(run("const e = new Date(2024, 1, 29); e.setFullYear(2025); [e.getMonth(), e.getDate()].join()"), "2,1");
    EXPECT_EQ(run("const h = new Date(0); h.setUTCHours(25, 61, 61, 1001); h.toISOString()"), "1970-01-02T02:02:02.001Z");
    EXPECT_EQ(run("[new Date('nope').getTime(), String(new Date(NaN)), new Date(8.64e15 + 1).getTime()].join()"), "NaN,Invalid Date,NaN");
    EXPECT_EQ(run("JSON.stringify({d: new Date(0)}) + typeof (new Date() + 1) + Object.prototype.toString.call(new Date())"), "{\"d\":\"1970-01-01T00:00:00.000Z\"}string[object Date]");
    EXPECT_EQ(run("[new Date('Nov 8, 2026 10:30 PM').getHours(), new Date('2026/11/08').getDate(), new Date(-62198755200000).toISOString()].join()"),
              "22,8,-000001-01-01T00:00:00.000Z");
}

TEST(Js, TypedArraysAndText) {
    EXPECT_EQ(run("const u = new Uint8Array([1, 2, 300, -1]); [u.join(), u.length, u[9], Object.prototype.toString.call(u)].join(' ')"), "1,2,44,255 4  [object Uint8Array]");
    EXPECT_EQ(run("new Uint8ClampedArray([1.5, 2.5, 300, -5, NaN]).join()"), "2,2,255,0,0");
    EXPECT_EQ(run("const b = new ArrayBuffer(8); const v = new DataView(b); v.setUint16(0, 0x1234); v.setFloat32(4, 1.5, true);"
                  "[new Uint8Array(b).join(), v.getUint16(0, true), new Int32Array(b, 4, 1)[0]].join(' ')"), "18,52,0,0,0,0,192,63 13330 1069547520");
    EXPECT_EQ(run("const m = new Int16Array([5, 1, 4]).map(x => x * 2).sort(); m.join() + (m instanceof Int16Array) + [...m.keys()]"), "2,8,10true0,1,2");
    EXPECT_EQ(run("const s = new Uint8Array(6); s.set([9, 8], 1); s.set(new Uint8Array([7]), 5); [s.join(), s.subarray(1, 3).join(), s.slice(-1).join()].join(' ')"),
              "0,9,8,0,0,7 9,8 7");
    EXPECT_EQ(run("const e = new TextEncoder().encode('h\\u00e9 \\u{1F600}'); e.length + ' ' + new TextDecoder().decode(e).length"), "8 5");
    EXPECT_EQ(run("new TextDecoder().decode(new Uint8Array([0x61, 0xff, 0x62]))"), "a\uFFFDb");
    EXPECT_EQ(run("const r = new ArrayBuffer(4, {maxByteLength: 16}); const t = new Uint8Array(r); r.resize(10); const m2 = r.transfer(); [t.length, r.detached, m2.byteLength].join()"),
              "0,true,10");
    EXPECT_EQ(run("try { new Uint8Array(-1) } catch (e) { e.name }"), "RangeError");
    EXPECT_EQ(run("const t = new Float64Array(2); t.x = 1; t[5] = 9; t['-0'] = 9; Object.keys(t).join() + ',' + t[5]"), "0,1,x,undefined");
}

TEST(Js, EvalAndFunction) {
    EXPECT_EQ(run("function f(a) { var local = 10; eval('var made = a + local; let inner = 5;'); return made + typeof inner; } f(2)"), "12undefined");
    EXPECT_EQ(run("[eval('1; 2 * 3'), (0, eval)('typeof f'), eval(42)].join()"), "6,undefined,42");
    EXPECT_EQ(run("function s() { 'use strict'; eval('var x = 1'); return typeof x; } s()"), "undefined");
    EXPECT_EQ(run("var g = 1; function h() { var g = 2; return (0, eval)('g'); } h()"), "1");
    EXPECT_EQ(run("new Function('a', 'b', 'return a + b')(2, 3)"), "5");
    EXPECT_EQ(run("try { new Function('a', '}); x = 1; ({') } catch (e) { e.name }"), "SyntaxError");
    EXPECT_EQ(run("try { eval('let = ;') } catch (e) { e.name }"), "SyntaxError");
    EXPECT_EQ(run("class A { m() { return eval('super.toString === Object.prototype.toString'); } } new A().m()"), "true");
    EXPECT_EQ(run("const G = Object.getPrototypeOf(function* () {}).constructor; new G('yield 7')().next().value"), "7");
    EXPECT_EQ(run("var n = 0; for (let i = 0; i < 1000; i++) eval('n += ' + (i % 3)); n"), "999");
}

TEST(Js, ProxyAndReflect) {
    EXPECT_EQ(run("const log = []; const p = new Proxy({a: 1}, {get(t, k, r) { log.push(String(k)); return k in t ? Reflect.get(t, k, r) : 'dflt'; },"
                  "set(t, k, v, r) { return Reflect.set(t, k, v * 2, r); }, has(t, k) { return k !== 'h' && k in t; }});"
                  "p.x = 5; [p.a, p.zz, p.x, 'a' in p, 'h' in p, log.join('')].join()"), "1,dflt,10,true,false,azzx");
    EXPECT_EQ(run("const p = new Proxy({}, {ownKeys() { return ['b', 'a']; }, getOwnPropertyDescriptor(t, k) { return {value: k, enumerable: true, configurable: true}; }});"
                  "Object.keys(p).join() + JSON.stringify({...p})"), "b,a{}");   // no get trap: the target has neither
    EXPECT_EQ(run("const f = new Proxy(function (a, b) { return a + b; }, {apply(t, th, args) { return t(...args) * 10; }, construct(t, args) { return {m: args[0]}; }});"
                  "[f(1, 2), new f(7).m, typeof f, Array.isArray(new Proxy([], {}))].join()"), "30,7,function,true");
    EXPECT_EQ(run("const {proxy, revoke} = Proxy.revocable({}, {}); revoke(); try { proxy.x } catch (e) { e.name }"), "TypeError");
    EXPECT_EQ(run("try { new Proxy(Object.freeze({k: 1}), {get() { return 2; }}).k } catch (e) { e.name }"), "TypeError");
    EXPECT_EQ(run("const o = Object.create(new Proxy({}, {get(t, k) { return k === 'q' ? 'via proto' : undefined; }})); o.q"), "via proto");
    EXPECT_EQ(run("class B { hi() { return 'hi ' + this.n; } } const i = new Proxy(new B(), {get(t, k, r) { return k === 'n' ? 'px' : Reflect.get(t, k, r); }});"
                  "i.hi() + (i instanceof B)"), "hi pxtrue");
    EXPECT_EQ(run("[Reflect.ownKeys({b: 1, 1: 2, [Symbol.iterator]: 0}).length, Reflect.defineProperty(Object.freeze({}), 'z', {value: 1}),"
                  "Reflect.getPrototypeOf([]) === Array.prototype, Reflect.construct(Date, [0]).getTime(), Reflect.apply(Math.max, null, [1, 3])].join()"),
              "3,false,true,0,3");
    EXPECT_EQ(run("const a = [0, 1]; Object.defineProperty(a, '1', {value: 1, configurable: false}); let threw = false; 'use strict';"
                  "try { Object.defineProperty(a, 'length', {value: 0}); } catch (e) { threw = true; } [threw, a.length].join()"), "true,2");
    EXPECT_EQ(run("const a = []; Object.defineProperty(a, '0', {value: 7}); const d = Object.getOwnPropertyDescriptor(a, '0'); [d.writable, d.enumerable, d.configurable, a[0]].join()"),
              "false,false,false,7");
    EXPECT_EQ(run("JSON.stringify(Object.groupBy([1, 2, 3, 4], x => x % 2 ? 'odd' : 'even')) + Map.groupBy([1, 2], x => x > 1).get(true)"),
              "{\"odd\":[1,3],\"even\":[2,4]}2");
}

TEST(Js, BigInt) {
    EXPECT_EQ(run("String(2n ** 100n) + ' ' + typeof 1n + ' ' + (12345678901234567890n * 3n) + ' ' + (-7n / 2n) + ' ' + (-7n % 2n)"),
              "1267650600228229401496703205376 bigint 37037036703703703670 -3 -1");
    EXPECT_EQ(run("[1n << 70n, -9n >> 1n, 7n & 3n, -1n | 4n, 5n ^ -2n, ~5n, 0xFEn, 1_000n, (255n).toString(16)].join()"),
              "1180591620717411303424,-5,3,-1,-5,-6,254,1000,ff");
    EXPECT_EQ(run("[1n == 1, 1n === 1, 2n > 1, 1n < 1.5, 10n > '9', 0n ? 1 : 0, BigInt('0x10'), BigInt(2 ** 60), Number(2n ** 64n)].join()"),
              "true,false,true,true,true,0,16,1152921504606846976,18446744073709552000");
    EXPECT_EQ(run("try { 1n + 1 } catch (e) { e.name }"), "TypeError");
    EXPECT_EQ(run("try { JSON.stringify(1n) } catch (e) { e.name }"), "TypeError");
    EXPECT_EQ(run("try { 1n / 0n } catch (e) { e.name }"), "RangeError");
    EXPECT_EQ(run("[BigInt.asIntN(8, 255n), BigInt.asUintN(8, -1n), BigInt.asIntN(64, 2n ** 63n)].join()"), "-1,255,-9223372036854775808");
    EXPECT_EQ(run("let i = 9n; i++; ++i; [i, new Map([[1n, 'm']]).get(1n), new Set([1n, 1n]).size].join()"), "11,m,1");
    EXPECT_EQ(run("const t = new BigInt64Array([1n, -2n, 2n ** 63n]); const d = new DataView(new ArrayBuffer(8)); d.setBigUint64(0, 0x0102030405060708n);"
                  "[t.join(), d.getUint8(0), d.getBigUint64(0, true).toString(16)].join(' ')"),
              "1,-2,-9223372036854775808 1 807060504030201");
    EXPECT_EQ(run("(+(false)) + ' ' + (!(true))"), "0 false");
}

// The scope and property storage behind the speed-ups: a call's scopes freed when it
// returns unless something can hold them, properties scanned or indexed, deletes as
// tombstones.
TEST(Js, ScopesAndPropertyStorage) {
    // A function that makes no closure gets its scopes freed; one that does keeps them.
    EXPECT_EQ(run("function leaf(a) { let b = a + 1; { let c = b * 2; return c; } }"
                  "function maker(a) { let b = a; return () => b++; }"
                  "const m = maker(5); let s = 0; for (let i = 0; i < 1000; i++) s += leaf(i); m(); [s, m(), m()].join()"),
              "1001000,6,7");
    // A direct eval can capture the scope too; an indirect one can't see it.
    EXPECT_EQ(run("function f(x) { return eval('() => x * 2'); } const g = f(21); g()"), "42");
    EXPECT_EQ(run("function f(x) { return (0, eval)('typeof x'); } f(1)"), "undefined");
    // Many properties: lookups past the scan limit, deletes, re-adds, and order kept.
    EXPECT_EQ(run("const o = {}; for (let i = 0; i < 100; i++) o['k' + i] = i;"
                  "for (let i = 0; i < 100; i += 2) delete o['k' + i];"
                  "o.k4 = 'again'; let sum = 0; for (const k in o) sum += typeof o[k] === 'number' ? o[k] : 0;"
                  "[Object.keys(o).length, Object.keys(o)[0], Object.keys(o).at(-1), sum, 'k2' in o, o.k99].join()"),
              "51,k1,k4,2500,false,99");
    // A small object: delete, then the same key back at the end.
    EXPECT_EQ(run("const o = {a: 1, b: 2, c: 3}; delete o.a; o.a = 4; delete o.b; JSON.stringify(o)"), "{\"c\":3,\"a\":4}");
    // Symbols and integer keys among many names.
    EXPECT_EQ(run("const s = Symbol('s'); const o = {[s]: 1}; for (let i = 0; i < 20; i++) o['p' + i] = i; o[2] = 'two'; o[1] = 'one';"
                  "delete o.p0; [Reflect.ownKeys(o).slice(0, 3).map(String).join('|'), o[s], Object.getOwnPropertySymbols(o).length].join()"),
              "1|2|p1,1,1");
    // for-in over a big object stays linear and visits each name once, own before inherited.
    EXPECT_EQ(run("const p = {z: 1, k3: 9}; const o = Object.create(p); for (let i = 0; i < 20000; i++) o['k' + i] = i;"
                  "let n = 0, z = 0; for (const k in o) { n++; if (k === 'z') z++; } [n, z].join()"),
              "20001,1");
}

// Engine safety found by test262 under a memory limit: iterators taken one value at a
// time and closed on failure, lengths checked before a result is built, accessors on
// array elements, and super with no prototype.
TEST(Js, IteratorsAndLimits) {
    // An endless iterator stops (and is closed) when the adder or mapFn throws.
    EXPECT_EQ(run("let closed = 0; const it = {[Symbol.iterator]() { return {next() { return {value: [1, 2], done: false}; }, return() { closed++; return {}; }}; }};"
                  "const set = Map.prototype.set; Map.prototype.set = function () { throw new Error('no'); };"
                  "let e1; try { new Map(it); } catch (e) { e1 = e.message; } Map.prototype.set = set;"
                  "let n = 0, e2; try { Array.from(it, () => { if (++n > 3) throw new Error('enough'); return n; }); } catch (e) { e2 = e.message; }"
                  "[e1, e2, closed, n].join()"),
              "no,enough,2,4");
    EXPECT_EQ(run("try { new Set(1); } catch (e) { e.name }"), "TypeError");
    // Array.from reads an array live, as its iterator does.
    EXPECT_EQ(run("const a = [1, 2, 3]; Array.from(a, (v, i) => { if (i === 0) a[2] = 30; return v; }).join()"), "1,2,30");
    // A length past 2^32 - 1 is a RangeError, not an allocation.
    EXPECT_EQ(run("const r = []; for (const f of [() => Array.prototype.map.call({length: Infinity}, x => x), () => Array.prototype.slice.call({length: 2 ** 40})])"
                  "  try { f(); r.push('no error'); } catch (e) { r.push(e.name); } r.join()"),
              "RangeError,RangeError");
    // An accessor on an existing element replaces the element.
    EXPECT_EQ(run("const a = ['foo', 'bar']; Object.defineProperty(a, 0, {get() { return 'got'; }}); [a[0], a.length, 'get' in Object.getOwnPropertyDescriptor(a, 0)].join()"),
              "got,2,true");
    EXPECT_EQ(run("try { Object.defineProperty([], 'length', {get() {}}); } catch (e) { e.name }"), "TypeError");
    // super.x where the home object has no prototype: a TypeError, after the right side.
    EXPECT_EQ(run("let n = 0; class C { static m() { super.x = n += 1; } } Object.setPrototypeOf(C, null);"
                  "let r; try { C.m(); } catch (e) { r = e.name; } [r, n].join()"),
              "TypeError,1");
}

// Collections while a script runs: at a loop's next round or a call's start, with the
// running stacks scanned and each call's arguments rooted. Here one runs every few
// hundred allocations, under everything that holds values mid-flight.
TEST(Js, CollectsWhileAScriptRuns) {
    Runtime rt;
    rt.setStepBudget(50'000'000);
    rt.setGcGrowth(300);
    const Runtime::Result r = rt.evaluate(
        // Closures kept and dropped, scopes a call frees itself, garbage per round.
        "const keep = []; let sum = 0;"
        "function leaf(a, b) { let t = {a, b}; return t.a + t.b; }"
        "for (let i = 0; i < 20000; i++) { const f = () => i; if (i % 1000 === 0) keep.push(f); sum += leaf(i, 1); [i, {i}]; }"
        // A generator suspended across collections, and one dropped half-way.
        "function* count() { let n = 0; while (true) yield {n: n++}; }"
        "const g = count(); for (let i = 0; i < 3000; i++) g.next(); count().next();"
        // A thrown object waiting while finally makes garbage; a WeakMap's live key.
        "let caught; try { try { throw {msg: 'kept'}; } finally { for (let i = 0; i < 2000; i++) ({i}); } } catch (e) { caught = e.msg; }"
        "const wk = {}; const wm = new WeakMap([[wk, 'v']]); for (let i = 0; i < 2000; i++) ({i});"
        // Arguments in a heap list while default parameters run and collect.
        "function late(a, b = (() => { for (let i = 0; i < 2000; i++) ({i}); return 2; })(), c) { return [a.x, b, c.y].join('/'); }"
        "const args = late.apply(null, [{x: 'ax'}, undefined, {y: 'cy'}]);"
        // A long forEach and a bound function.
        "let fe = 0; Array.from({length: 3000}, (_, i) => ({i})).forEach(o => { fe += o.i; ({o}); });"
        "const bound = function (p, q) { for (let i = 0; i < 2000; i++) ({i}); return p.v + q.v; }.bind(null, {v: 1});"
        "[keep.length, keep[7](), sum, g.next().value.n, caught, wm.get(wk), args, fe, bound({v: 2})].join()");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(utf8(rt.toStringValue(r.value)), "20,7000,200010000,3000,kept,v,ax/2/cy,4498500,3");
    EXPECT_GT(rt.mid_script_collections, 50u);
    // What the loop made and dropped is gone already, not waiting for the script's end.
    EXPECT_LT(rt.heapSize(), 50'000u);
}

TEST(Js, DivisionAfterPostfixIncrement) {
    EXPECT_EQ(run("var d = 10, k = [1, 2, 3, 4]; var y = d++ / 2; var z = k[d-- / 5 >> 1]; y + ',' + z + ',' + d"), "5,2,10");
}

TEST(Js, RegexAfterAStatementHead) {
    EXPECT_EQ(run("var a = [], s = ['and[1]', 'x', 'not[2]'];"
                  "for (let t of s) /and\\[\\d+\\]/.test(t) ? a.push('a') : /not\\[\\d+\\]/.test(t) && a.push('n');"
                  "if (true) /x/.test('x') && a.push('i');"
                  "var i = 0; while (i++ < 1) /y/.test('y') && a.push('w');"
                  "var b = (4) / 2;"
                  "a.join('') + b"), "aniw2");
}

TEST(Js, ATaggedTemplateSiteGivesTheSameStringsEachTime) {
    // lit tells templates apart by this identity, and rebuilds the DOM when it differs.
    EXPECT_EQ(run("function tag(s) { return s; }"
                  "function f(x) { return tag`a${x}b`; }"
                  "const a = f(1), b = f(2), g = () => tag`q`;"
                  "[a === b, a.raw === b.raw, g() === g(), tag`q` === g(), a.join()].join()"),
              "true,true,true,false,a,b");
}

TEST(Js, NativesCalledInsideAConstructorAreCalls) {
    // A Promise executor runs while Promise's constructor does: String() there is still a call.
    EXPECT_EQ(run("let t; new Promise(() => { t = typeof String(1) + typeof Number('2') + typeof Boolean(0); }); "
                  "const m = new Map([[1, typeof String(3)]]); t + m.get(1)"),
              "stringnumberbooleanstring");
}

TEST(Js, ArgumentsCallee) {
    EXPECT_EQ(run("function f() { return arguments.callee; } "
                  "function g() { 'use strict'; try { return arguments.callee; } catch (e) { return e instanceof TypeError; } } "
                  "const keys = (function () { return Object.keys(arguments).join(); })(1, 2); "
                  "[f() === f, g(), keys].join()"),
              "true,true,0,1");
}

TEST(Js, WithStatement) {
    EXPECT_EQ(run("var o = { a: 1, f() { return this === o; } }, a = 'outer', b = 2; var out = [];"
                  "with (o) { out.push(a, b, f(), typeof a); a = 5; b = 3; }"
                  "out.push(o.a, b, a);"
                  "var u = { x: 1, [Symbol.unscopables]: { x: true } }, x = 'mine';"
                  "with (u) out.push(x);"
                  "out.join()"),
              "1,2,true,number,5,3,outer,mine");
    EXPECT_EQ(run("'use strict'; with ({}) {}"), "throws: SyntaxError: with isn't allowed in strict mode (line 1)");
}

TEST(Js, ARegexMayFollowAStatementsClosingBrace) {
    // Minified code: function f(){...}/^loade|c/.test(document.readyState) (Twitter's
    // widgets.js, Tistory). After an expression's } it still divides.
    EXPECT_EQ(run("var out = []; function f(){}/a/.test('a') && out.push(1); {}/b/g.test('b') && out.push(2);"
                  "class C{}/c/.test('c') && out.push(3); switch (1) {}/d/.test('d') && out.push(4);"
                  "var h = function(){ return 3 }/2; out.push(typeof h, ({}).x/1);"
                  "out.join()"),
              "1,2,3,4,number,NaN");
}

TEST(Js, DeleteThroughAShortCircuitedChain) {
    // delete a?.b.c with a nullish: true, nothing deleted (it used to escape the engine).
    EXPECT_EQ(run("var a = null, o = { b: { c: 1 } }; [delete a?.b.c, delete o?.b.c, 'c' in o.b, delete a?.b].join()"), "true,true,false,true");
}

TEST(Js, StringsGrowInPlaceAndEvalKeepsItsValue) {
    // s += x on a string nothing else holds appends where it is (it was a copy each time:
    // quadratic). What the right side does to s first still counts.
    EXPECT_EQ(run("function f() { let s = ''; for (let i = 0; i < 1000; i++) s += 'ab'; let t = s; s += 'c'; return [s.length, t.length].join(); } f()"), "2001,2000");
    EXPECT_EQ(run("var x = 'a'; x += (x = 'q', 'b'); x"), "ab");
    EXPECT_EQ(run("eval(\"var y = 'a'; y += 'b'\")"), "ab");
    EXPECT_EQ(run("var z = 'p'; var w = z; z += z; [z, w].join()"), "pp,p");
}

TEST(Js, ErrorStacksListTheCalls) {
    EXPECT_EQ(run("function inner() { return new Error('x').stack; } function outer() { return inner(); }"
                  "var s = outer(); [s.split('\\n').length, /at inner \\(/.test(s), /at outer \\(/.test(s)].join()"), "4,true,true");
}
