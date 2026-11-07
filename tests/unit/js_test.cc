// Our JavaScript engine: the language and its library, script in, value or error out.

#include <gtest/gtest.h>

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
    EXPECT_EQ(run("while (true) {}"), "throws: Stopped: the script ran too long");
    EXPECT_EQ(run("for (;;) { try { } catch (e) { } }"), "throws: Stopped: the script ran too long");
    std::string deep(5000, '(');
    deep += "1" + std::string(5000, ')');
    EXPECT_EQ(run(deep), "throws: SyntaxError: too deeply nested (line 1)");
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
    EXPECT_EQ(got, "log:a 1 {\"x\":[2]};error:TypeError: t;");
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
