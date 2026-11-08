#pragma once

// BigInt arithmetic, our own: sign and magnitude in 32-bit limbs, and the operations the
// language has (two's complement for the bitwise ones). A BigInt value keeps its digits
// in the string slot every Value has (encoded, never text), so values don't grow.
// Internal to the engine.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fbx::xweb::js::bigint {

struct Big {
    bool neg = false;
    std::vector<uint32_t> mag;   // little-endian, no leading zeros; zero is empty
    [[nodiscard]] bool isZero() const noexcept { return mag.empty(); }
};

inline constexpr size_t kMaxBits = 1 << 20;   // larger than this is a RangeError

// To and from a value's storage.
std::u16string encode(const Big& b);
Big decode(const std::u16string& s);

// StringToBigInt: decimal, or 0x/0o/0b; surrounding white space allowed; nothing if invalid.
std::optional<Big> parse(std::u16string_view text);
// A literal's digits ("123", "0x1f", "1_000" with separators already removed).
std::optional<Big> parseLiteral(std::u16string_view text);
std::optional<Big> fromDouble(double d);   // only an integral number
double toDouble(const Big& b);
std::u16string toString(const Big& b, int radix = 10);
Big fromInt64(int64_t v);
Big fromUint64(uint64_t v);
int64_t toInt64(const Big& b);     // modulo 2^64
uint64_t toUint64(const Big& b);

int compare(const Big& a, const Big& b);
int compareWithDouble(const Big& a, double d);   // -1, 0, 1; 2 for NaN
Big add(const Big& a, const Big& b);
Big sub(const Big& a, const Big& b);
Big mul(const Big& a, const Big& b);
// Truncating, as the language's / and %; false on division by zero.
bool divmod(const Big& a, const Big& b, Big& q, Big& r);
Big negate(const Big& a);
Big bitAnd(const Big& a, const Big& b);
Big bitOr(const Big& a, const Big& b);
Big bitXor(const Big& a, const Big& b);
Big bitNot(const Big& a);
Big shiftLeft(const Big& a, int64_t n);    // negative n shifts right (rounding down)
Big asIntN(const Big& a, uint64_t bits);
Big asUintN(const Big& a, uint64_t bits);
size_t bitLength(const Big& a);

}  // namespace fbx::xweb::js::bigint
