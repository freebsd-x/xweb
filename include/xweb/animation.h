#pragma once

// CSS transitions and animations. Styling works out what an element's are (AnimationSpec,
// with each keyframe's style, and TransitionSpec, in its ComputedStyle); the Animator
// keeps what's running, and lays the moving values over a StyledDocument's, whose own
// styles stay as the cascade made them: time passing restyles nothing.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xweb/css.h"
#include "xweb/dom.h"

namespace fbx::xweb {

struct ComputedStyle;
class StyledDocument;

struct Easing {
    enum class Kind : uint8_t { Linear, Cubic, Steps } kind = Kind::Cubic;
    float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1;   // ease
    int steps = 1;
    uint8_t jump = 1;   // steps: 0 at the start, 1 at the end, 2 neither, 3 both
    [[nodiscard]] float at(float t) const noexcept;
};
// One from a value's tokens, at i (moved past it). False if it isn't one.
bool easingAt(const std::vector<css::Token>& v, size_t& i, Easing& out);

// What moves, in groups that move together.
enum MovingBits : uint32_t {
    kMoveOpacity = 1u << 0, kMoveTransform = 1u << 1, kMoveTranslate = 1u << 2, kMoveRotate = 1u << 3, kMoveScale = 1u << 4,
    kMoveColor = 1u << 5, kMoveBackground = 1u << 6, kMoveWidth = 1u << 7, kMoveHeight = 1u << 8, kMoveMinMax = 1u << 9,
    kMoveMargin = 1u << 10, kMovePadding = 1u << 11, kMoveTop = 1u << 12, kMoveRight = 1u << 13, kMoveBottom = 1u << 14, kMoveLeft = 1u << 15,
    kMoveBorderColor = 1u << 16, kMoveVisibility = 1u << 17, kMoveFontSize = 1u << 18, kMoveRadius = 1u << 19, kMoveFilter = 1u << 20,
    kMoveMaxHeight = 1u << 21, kMoveMaxWidth = 1u << 22,
    kMoveAll = (1u << 23) - 1,
};
[[nodiscard]] uint32_t movingBits(const std::string& property);
// Which of them differ; those that can't be gone between (auto to a length) aren't said to.
[[nodiscard]] uint32_t movingDiff(const ComputedStyle& a, const ComputedStyle& b);
// `out`'s made what's `t` of the way from a's to b's.
void moveBetween(ComputedStyle& out, const ComputedStyle& a, const ComputedStyle& b, float t, uint32_t bits);

struct TransitionSpec {
    uint32_t bits = kMoveAll;
    std::string property = "all";
    float duration = 0, delay = 0;   // seconds
    Easing easing;
};
using TransitionList = std::vector<TransitionSpec>;

struct AnimationSpec {
    struct Frame {
        float offset = 0;
        bool has_easing = false;
        Easing easing;                                // to the next one
        uint32_t bits = 0;
        std::shared_ptr<const ComputedStyle> style;   // the element's, with this frame's declarations
    };
    std::string name;
    float duration = 0, delay = 0, iterations = 1;    // (infinity: for ever)
    enum class Direction : uint8_t { Normal, Reverse, Alternate, AlternateReverse } direction = Direction::Normal;
    bool fill_forwards = false, fill_backwards = false, paused = false;
    Easing easing;
    std::shared_ptr<const std::vector<Frame>> frames;   // in order; empty: there are no such keyframes
};
using AnimationList = std::vector<AnimationSpec>;

class Animator {
public:
    struct Event {
        const Node* node = nullptr;
        int pseudo = 0;               // 1 ::before, 2 ::after
        std::string type, name;       // "transitionend" and its property, "animationend" and its name
        float elapsed = 0;
    };
    // `now` styled, in place of `before` (null: nothing was): what changed starts moving.
    void styled(StyledDocument& now, const StyledDocument* before, double seconds);
    // Whether anything would start or stop by styled(now, before): nothing's done.
    [[nodiscard]] bool wouldStart(const StyledDocument& now, const StyledDocument& before) const;
    // Time has passed: what's moving laid over the document's styles again. True if any
    // value came out different.
    bool tick(StyledDocument& doc, double seconds);
    // Laid over another document of the same page, as they are now.
    void cover(StyledDocument& doc) const;
    // Something is moving (or about to): `finite` if it isn't only what goes on for ever.
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] bool finiteRunning() const noexcept { return finite_running_; }
    [[nodiscard]] std::vector<const Node*> moving() const;
    std::vector<Event> takeEvents();
    void clear();

private:
    using Key = std::pair<const Node*, int>;
    struct Transition {
        uint32_t bit = 0;
        std::string property;
        std::shared_ptr<const ComputedStyle> from, to;
        double start = 0;
        float duration = 0, delay = 0;
        Easing easing;
        bool begun = false;
    };
    struct Playing {
        std::string name;
        double start = 0;
        double held = -1;      // paused, this far in
        bool begun = false, ended = false;
        int iteration = 0;
    };
    struct Entry {
        std::shared_ptr<const ComputedStyle> base;   // (a copy: its document may go)
        const ComputedStyle* base_at = nullptr;
        std::vector<Transition> transitions;
        std::vector<Playing> playing;
        std::shared_ptr<const ComputedStyle> shown;  // what's laid over; null: nothing is
    };
    bool advance(const Key& key, Entry& e, double seconds);
    std::map<Key, Entry> entries_;
    std::vector<Event> events_;
    bool running_ = false, finite_running_ = false;
};

}  // namespace fbx::xweb
