// BigInt arithmetic: schoolbook algorithms over 32-bit limbs, which is plenty for the
// sizes pages use (ids, 64-bit values, hashes).

#include "bigint.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fbx::xweb::js::bigint {

namespace {

void trim(std::vector<uint32_t>& m) {
    while (!m.empty() && m.back() == 0) m.pop_back();
}

Big make(bool neg, std::vector<uint32_t> mag) {
    trim(mag);
    Big b;
    b.neg = neg && !mag.empty();
    b.mag = std::move(mag);
    return b;
}

int compareMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

std::vector<uint32_t> addMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out(std::max(a.size(), b.size()) + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        uint64_t s = carry;
        if (i < a.size()) s += a[i];
        if (i < b.size()) s += b[i];
        out[i] = static_cast<uint32_t>(s);
        carry = s >> 32;
    }
    trim(out);
    return out;
}

// a - b, with |a| >= |b|.
std::vector<uint32_t> subMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out(a.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int64_t d = static_cast<int64_t>(a[i]) - borrow - (i < b.size() ? static_cast<int64_t>(b[i]) : 0);
        borrow = d < 0 ? 1 : 0;
        if (d < 0) d += (int64_t{1} << 32);
        out[i] = static_cast<uint32_t>(d);
    }
    trim(out);
    return out;
}

std::vector<uint32_t> mulSmall(const std::vector<uint32_t>& a, uint32_t m, uint32_t addend) {
    std::vector<uint32_t> out(a.size() + 1, 0);
    uint64_t carry = addend;
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t p = static_cast<uint64_t>(a[i]) * m + carry;
        out[i] = static_cast<uint32_t>(p);
        carry = p >> 32;
    }
    out[a.size()] = static_cast<uint32_t>(carry);
    trim(out);
    return out;
}

uint32_t divSmall(std::vector<uint32_t>& a, uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = a.size(); i-- > 0;) {
        const uint64_t cur = (rem << 32) | a[i];
        a[i] = static_cast<uint32_t>(cur / d);
        rem = cur % d;
    }
    trim(a);
    return static_cast<uint32_t>(rem);
}

// Knuth's algorithm D, for |a| / |b|.
void divmodMag(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, std::vector<uint32_t>& q, std::vector<uint32_t>& r) {
    if (compareMag(a, b) < 0) {
        q.clear();
        r = a;
        return;
    }
    if (b.size() == 1) {
        q = a;
        const uint32_t rem = divSmall(q, b[0]);
        r.clear();
        if (rem) r.push_back(rem);
        return;
    }
    const int s = __builtin_clz(b.back());
    std::vector<uint32_t> u(a.size() + 1, 0), v(b.size(), 0);
    for (size_t i = b.size(); i-- > 0;) v[i] = (b[i] << s) | (s && i ? b[i - 1] >> (32 - s) : 0);
    for (size_t i = a.size(); i-- > 0;) u[i] = (a[i] << s) | (s && i ? a[i - 1] >> (32 - s) : 0);
    u[a.size()] = s ? a.back() >> (32 - s) : 0;
    const size_t n = v.size(), m = a.size() - n;
    q.assign(m + 1, 0);
    for (size_t j = m + 1; j-- > 0;) {
        const uint64_t num = (static_cast<uint64_t>(u[j + n]) << 32) | u[j + n - 1];
        uint64_t qhat = num / v[n - 1];
        uint64_t rhat = num % v[n - 1];
        while (qhat >= (uint64_t{1} << 32) || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
            --qhat;
            rhat += v[n - 1];
            if (rhat >= (uint64_t{1} << 32)) break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            const int64_t t = static_cast<int64_t>(u[i + j]) - borrow - static_cast<int64_t>(p & 0xFFFFFFFFu);
            u[i + j] = static_cast<uint32_t>(t);
            borrow = t < 0 ? 1 : 0;
        }
        const int64_t t = static_cast<int64_t>(u[j + n]) - borrow - static_cast<int64_t>(carry);
        u[j + n] = static_cast<uint32_t>(t);
        if (t < 0) {
            --qhat;
            uint64_t c = 0;
            for (size_t i = 0; i < n; ++i) {
                const uint64_t sum = static_cast<uint64_t>(u[i + j]) + v[i] + c;
                u[i + j] = static_cast<uint32_t>(sum);
                c = sum >> 32;
            }
            u[j + n] += static_cast<uint32_t>(c);
        }
        q[j] = static_cast<uint32_t>(qhat);
    }
    trim(q);
    r.assign(n, 0);
    for (size_t i = 0; i < n; ++i) r[i] = (u[i] >> s) | (s && i + 1 < u.size() ? static_cast<uint32_t>(static_cast<uint64_t>(u[i + 1]) << (32 - s)) : 0);
    trim(r);
}

// Two's complement limbs of `a`, `words` long.
std::vector<uint32_t> twos(const Big& a, size_t words) {
    std::vector<uint32_t> out(words, 0);
    for (size_t i = 0; i < a.mag.size() && i < words; ++i) out[i] = a.mag[i];
    if (a.neg) {
        uint64_t carry = 1;
        for (auto& w : out) {
            const uint64_t v = static_cast<uint64_t>(~w) + carry;
            w = static_cast<uint32_t>(v);
            carry = v >> 32;
        }
    }
    return out;
}

Big fromTwos(std::vector<uint32_t> w) {
    const bool neg = !w.empty() && (w.back() & 0x80000000u);
    if (neg) {
        uint64_t carry = 1;
        for (auto& x : w) {
            const uint64_t v = static_cast<uint64_t>(~x) + carry;
            x = static_cast<uint32_t>(v);
            carry = v >> 32;
        }
    }
    return make(neg, std::move(w));
}

template <typename F>
Big bitwise(const Big& a, const Big& b, F op) {
    const size_t words = std::max(a.mag.size(), b.mag.size()) + 1;
    std::vector<uint32_t> x = twos(a, words), y = twos(b, words);
    for (size_t i = 0; i < words; ++i) x[i] = op(x[i], y[i]);
    return fromTwos(std::move(x));
}

int digitOf(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'z') return c - u'a' + 10;
    if (c >= u'A' && c <= u'Z') return c - u'A' + 10;
    return 99;
}

std::optional<Big> digits(std::u16string_view s, int radix) {
    if (s.empty()) return std::nullopt;
    std::vector<uint32_t> mag;
    for (char16_t c : s) {
        const int d = digitOf(c);
        if (d >= radix) return std::nullopt;
        mag = mulSmall(mag, static_cast<uint32_t>(radix), static_cast<uint32_t>(d));
        if (mag.size() * 32 > kMaxBits + 64) return std::nullopt;
    }
    return make(false, std::move(mag));
}

bool space(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF ||
           c == 0x2028 || c == 0x2029 || (c >= 0x2000 && c <= 0x200A) || c == 0x1680 || c == 0x202F || c == 0x205F || c == 0x3000;
}

}  // namespace

std::u16string encode(const Big& b) {
    std::u16string s;
    s.reserve(1 + b.mag.size() * 2);
    s += static_cast<char16_t>(b.neg ? 1 : 0);
    for (uint32_t w : b.mag) {
        s += static_cast<char16_t>(w & 0xFFFF);
        s += static_cast<char16_t>(w >> 16);
    }
    return s;
}

Big decode(const std::u16string& s) {
    Big b;
    if (s.empty()) return b;
    b.neg = s[0] != 0;
    for (size_t i = 1; i + 1 < s.size(); i += 2) b.mag.push_back(static_cast<uint32_t>(s[i]) | (static_cast<uint32_t>(s[i + 1]) << 16));
    return b;
}

std::optional<Big> parse(std::u16string_view text) {
    size_t b = 0, e = text.size();
    while (b < e && space(text[b])) ++b;
    while (e > b && space(text[e - 1])) --e;
    std::u16string_view s = text.substr(b, e - b);
    if (s.empty()) return Big{};
    if (s.size() > 2 && s[0] == u'0' && (s[1] == u'x' || s[1] == u'X')) return digits(s.substr(2), 16);
    if (s.size() > 2 && s[0] == u'0' && (s[1] == u'o' || s[1] == u'O')) return digits(s.substr(2), 8);
    if (s.size() > 2 && s[0] == u'0' && (s[1] == u'b' || s[1] == u'B')) return digits(s.substr(2), 2);
    bool neg = false;
    if (s[0] == u'+' || s[0] == u'-') {
        neg = s[0] == u'-';
        s.remove_prefix(1);
    }
    auto r = digits(s, 10);
    if (r && neg) *r = negate(*r);
    return r;
}

std::optional<Big> parseLiteral(std::u16string_view s) {
    if (s.size() > 2 && s[0] == u'0' && (s[1] == u'x' || s[1] == u'X')) return digits(s.substr(2), 16);
    if (s.size() > 2 && s[0] == u'0' && (s[1] == u'o' || s[1] == u'O')) return digits(s.substr(2), 8);
    if (s.size() > 2 && s[0] == u'0' && (s[1] == u'b' || s[1] == u'B')) return digits(s.substr(2), 2);
    return digits(s, 10);
}

std::optional<Big> fromDouble(double d) {
    if (!std::isfinite(d) || d != std::trunc(d)) return std::nullopt;
    const bool neg = d < 0;
    d = std::fabs(d);
    std::vector<uint32_t> mag;
    int exp = 0;
    const double m = std::frexp(d, &exp);   // d = m * 2^exp, 0.5 <= m < 1
    if (d == 0) return Big{};
    const uint64_t mant = static_cast<uint64_t>(std::ldexp(m, 53));
    Big b = fromUint64(mant);
    b = shiftLeft(b, exp - 53);
    if (neg) b = negate(b);
    return b;
}

double toDouble(const Big& b) {
    if (b.isZero()) return 0;
    // Round to nearest from the top 64 bits and a sticky bit, then scale.
    const size_t bits = bitLength(b);
    if (bits > 1100) return b.neg ? -INFINITY : INFINITY;
    double r = 0;
    if (bits <= 64) {
        r = static_cast<double>(toUint64(Big{false, b.mag}));
    } else {
        const Big top = shiftLeft(Big{false, b.mag}, -static_cast<int64_t>(bits - 64));
        uint64_t t = toUint64(top);
        // Sticky: any bit below those 64.
        bool sticky = false;
        const size_t below = bits - 64;
        for (size_t i = 0; i < below / 32 && !sticky; ++i) sticky = b.mag[i] != 0;
        if (!sticky && below % 32) sticky = (b.mag[below / 32] & ((1u << (below % 32)) - 1)) != 0;
        // Round 64 bits to 53, ties to even, with the sticky bit.
        const uint64_t lower = t & 0x7FF;
        uint64_t upper = t >> 11;
        if (lower > 0x400 || (lower == 0x400 && (sticky || (upper & 1)))) ++upper;
        else if (lower == 0x400 && !sticky && !(upper & 1)) {}
        r = std::ldexp(static_cast<double>(upper), static_cast<int>(below + 11));
    }
    return b.neg ? -r : r;
}

std::u16string toString(const Big& b, int radix) {
    if (b.isZero()) return u"0";
    std::vector<uint32_t> m = b.mag;
    std::u16string out;
    static const char16_t* d = u"0123456789abcdefghijklmnopqrstuvwxyz";
    while (!m.empty()) out += d[divSmall(m, static_cast<uint32_t>(radix))];
    if (b.neg) out += u'-';
    std::reverse(out.begin(), out.end());
    return out;
}

Big fromUint64(uint64_t v) {
    std::vector<uint32_t> m{static_cast<uint32_t>(v), static_cast<uint32_t>(v >> 32)};
    return make(false, std::move(m));
}

Big fromInt64(int64_t v) {
    if (v >= 0) return fromUint64(static_cast<uint64_t>(v));
    return negate(fromUint64(static_cast<uint64_t>(-(v + 1)) + 1));
}

uint64_t toUint64(const Big& b) {
    const std::vector<uint32_t> w = twos(b, 2);
    return static_cast<uint64_t>(w[0]) | (static_cast<uint64_t>(w[1]) << 32);
}

int64_t toInt64(const Big& b) { return static_cast<int64_t>(toUint64(b)); }

int compare(const Big& a, const Big& b) {
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    const int c = compareMag(a.mag, b.mag);
    return a.neg ? -c : c;
}

int compareWithDouble(const Big& a, double d) {
    if (std::isnan(d)) return 2;
    if (std::isinf(d)) return d > 0 ? -1 : 1;
    const double t = std::trunc(d);
    const Big bt = *fromDouble(t);
    const int c = compare(a, bt);
    if (c != 0) return c;
    if (d > t) return -1;
    if (d < t) return 1;
    return 0;
}

Big add(const Big& a, const Big& b) {
    if (a.neg == b.neg) return make(a.neg, addMag(a.mag, b.mag));
    const int c = compareMag(a.mag, b.mag);
    if (c == 0) return Big{};
    return c > 0 ? make(a.neg, subMag(a.mag, b.mag)) : make(b.neg, subMag(b.mag, a.mag));
}

Big negate(const Big& a) { return make(!a.neg, a.mag); }

Big sub(const Big& a, const Big& b) { return add(a, negate(b)); }

Big mul(const Big& a, const Big& b) {
    if (a.isZero() || b.isZero()) return Big{};
    std::vector<uint32_t> out(a.mag.size() + b.mag.size(), 0);
    for (size_t i = 0; i < a.mag.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.mag.size(); ++j) {
            const uint64_t p = static_cast<uint64_t>(a.mag[i]) * b.mag[j] + out[i + j] + carry;
            out[i + j] = static_cast<uint32_t>(p);
            carry = p >> 32;
        }
        size_t k = i + b.mag.size();
        while (carry) {
            const uint64_t s = static_cast<uint64_t>(out[k]) + carry;
            out[k++] = static_cast<uint32_t>(s);
            carry = s >> 32;
        }
    }
    return make(a.neg != b.neg, std::move(out));
}

bool divmod(const Big& a, const Big& b, Big& q, Big& r) {
    if (b.isZero()) return false;
    std::vector<uint32_t> qm, rm;
    divmodMag(a.mag, b.mag, qm, rm);
    q = make(a.neg != b.neg, std::move(qm));
    r = make(a.neg, std::move(rm));
    return true;
}

Big bitAnd(const Big& a, const Big& b) { return bitwise(a, b, [](uint32_t x, uint32_t y) { return x & y; }); }
Big bitOr(const Big& a, const Big& b) { return bitwise(a, b, [](uint32_t x, uint32_t y) { return x | y; }); }
Big bitXor(const Big& a, const Big& b) { return bitwise(a, b, [](uint32_t x, uint32_t y) { return x ^ y; }); }
Big bitNot(const Big& a) { return sub(negate(a), fromInt64(1)); }

size_t bitLength(const Big& a) {
    if (a.mag.empty()) return 0;
    return (a.mag.size() - 1) * 32 + (32 - static_cast<size_t>(__builtin_clz(a.mag.back())));
}

Big shiftLeft(const Big& a, int64_t n) {
    if (a.isZero() || n == 0) return a;
    if (n > 0) {
        const size_t words = static_cast<size_t>(n / 32), bits = static_cast<size_t>(n % 32);
        std::vector<uint32_t> out(a.mag.size() + words + 1, 0);
        for (size_t i = 0; i < a.mag.size(); ++i) {
            const uint64_t v = static_cast<uint64_t>(a.mag[i]) << bits;
            out[i + words] |= static_cast<uint32_t>(v);
            out[i + words + 1] |= static_cast<uint32_t>(v >> 32);
        }
        return make(a.neg, std::move(out));
    }
    // Right: rounding toward negative infinity, as >> on BigInts does.
    const uint64_t m = static_cast<uint64_t>(-n);
    const size_t words = static_cast<size_t>(m / 32), bits = static_cast<size_t>(m % 32);
    if (words >= a.mag.size()) return a.neg ? fromInt64(-1) : Big{};
    bool lost = false;
    for (size_t i = 0; i < words && !lost; ++i) lost = a.mag[i] != 0;
    if (bits && (a.mag[words] & ((1u << bits) - 1))) lost = true;
    std::vector<uint32_t> out(a.mag.size() - words, 0);
    for (size_t i = 0; i < out.size(); ++i) {
        uint64_t v = a.mag[i + words] >> bits;
        if (bits && i + words + 1 < a.mag.size()) v |= static_cast<uint64_t>(a.mag[i + words + 1]) << (32 - bits);
        out[i] = static_cast<uint32_t>(v);
    }
    Big r = make(a.neg, std::move(out));
    if (a.neg && lost) r = sub(r, fromInt64(1));
    return r;
}

Big asUintN(const Big& a, uint64_t bits) {
    if (bits == 0) return Big{};
    const size_t words = static_cast<size_t>((bits + 31) / 32);
    if (!a.neg && bitLength(a) <= bits) return a;
    std::vector<uint32_t> w = twos(a, words);
    if (bits % 32) w.back() &= (1u << (bits % 32)) - 1;
    return make(false, std::move(w));
}

Big asIntN(const Big& a, uint64_t bits) {
    if (bits == 0) return Big{};
    if (bits > bitLength(a) + 1) return a;   // it fits as it is
    const Big u = asUintN(a, bits);
    // Top bit set: subtract 2^bits.
    if (bitLength(u) == bits) return sub(u, shiftLeft(fromInt64(1), static_cast<int64_t>(bits)));
    return u;
}

}  // namespace fbx::xweb::js::bigint
