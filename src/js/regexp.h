#pragma once

// Regular expressions, our own: the ECMAScript pattern language (with the web's
// Annex B leniency when not in unicode mode) compiled to a small program, and a
// backtracking matcher that runs it over UTF-16 text. Internal to the engine.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbx::xweb::js {

class Runtime;

namespace regex {

struct Flags {
    bool has_indices = false;   // d
    bool global = false;        // g
    bool ignore_case = false;   // i
    bool multiline = false;     // m
    bool dot_all = false;       // s
    bool unicode = false;       // u
    bool unicode_sets = false;  // v
    bool sticky = false;        // y
};

// The flags a string names, or nothing if it repeats one or names an unknown one.
[[nodiscard]] std::optional<Flags> parseFlags(std::u16string_view s);
[[nodiscard]] std::u16string flagsString(const Flags& f);

struct Program;

// The pattern compiled, or nothing with `error` saying why.
[[nodiscard]] std::shared_ptr<const Program> compile(std::u16string_view pattern, const Flags& flags, std::string& error);

// Capturing groups, not counting the whole match.
[[nodiscard]] size_t groupCount(const Program& p);
// Named groups and their numbers, in the order the pattern has them.
[[nodiscard]] const std::vector<std::pair<std::u16string, int>>& groupNames(const Program& p);
[[nodiscard]] const Flags& flagsOf(const Program& p);

// Looks for a match starting at `start` (only there, if sticky). On success `caps`
// holds 2 * (groupCount + 1) offsets, -1 for a group that took no part. Long runs
// take from the runtime's step budget; a match too complex to finish throws.
bool exec(Runtime& rt, const Program& p, std::u16string_view input, size_t start, bool sticky, std::vector<int>& caps);

// Limits a page can't push past.
inline constexpr size_t kMaxPatternLength = 1 << 20;
inline constexpr size_t kMaxProgramSize = 1 << 20;
inline constexpr size_t kMaxNesting = 500;
inline constexpr size_t kMaxBacktrack = 4 << 20;

}  // namespace regex
}  // namespace fbx::xweb::js
