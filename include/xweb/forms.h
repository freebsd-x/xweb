#pragma once

// Forms: what each control holds while the page is open, and what submitting a form
// sends, as the HTML spec has it (constructing the entry list, then encoding it).

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xweb/dom.h"

namespace fbx::xweb {

enum class ControlKind {
    None,
    Text,       // text, search, email, url, tel, number and the rest typed into
    Password,
    Checkbox,
    Radio,
    Submit,     // <input type=submit>, <button> (type submit, the default)
    Image,      // <input type=image>: a submit button
    Reset,      // <input type=reset>, <button type=reset>
    Button,     // <input type=button>, <button type=button>: does nothing without scripts
    Hidden,
    File,       // not supported: never submitted with a file
    TextArea,
    Select,
};

[[nodiscard]] ControlKind controlKind(const Node* n);
[[nodiscard]] inline bool isTypedInto(ControlKind k) { return k == ControlKind::Text || k == ControlKind::Password || k == ControlKind::TextArea; }
// Disabled itself, or inside a disabled <fieldset> (but not in its first <legend>).
[[nodiscard]] bool isDisabled(const Node* control);
// The form it belongs to: its form="" attribute's, or the nearest <form> around it.
[[nodiscard]] const Node* formOwner(const Node* control);
// A <select>'s options, optgroups looked into, in order.
[[nodiscard]] std::vector<const Node*> optionsOf(const Node* select);
// What a <label> labels: its for="" target, or the first control inside it.
[[nodiscard]] const Node* labelledControl(const Node* label);
// An <option>'s value: its value="" or else its text, whitespace collapsed.
[[nodiscard]] std::string optionValue(const Node* option);
[[nodiscard]] std::string optionLabel(const Node* option);

class FormState {
public:
    // What's in it now: what was typed, or else what the page said.
    [[nodiscard]] std::string value(const Node* control) const;
    [[nodiscard]] bool checked(const Node* control) const;
    // Index into optionsOf(select), or -1 for none.
    [[nodiscard]] int selected(const Node* select) const;

    void setValue(const Node* control, std::string value);
    // A checkbox flips; a radio button turns on and its group's others off.
    void activate(const Node* control);
    void select(const Node* select, int index);
    // Everything in the form back to what the page said.
    void reset(const Node* form);

    // The control being typed into, and where in it (a byte offset into value()).
    const Node* focused = nullptr;
    size_t caret = 0;

private:
    struct State {
        bool has_value = false;
        std::string value;
        bool has_checked = false;
        bool checked = false;
        int selected = -2;   // -2: as the page said
    };
    std::unordered_map<const Node*, State> states_;
};

struct FormSubmission {
    std::string url;   // where to go: for GET, with the data as its query
    bool post = false;
    std::string content_type;
    std::string body;
};

// What submitting `form` with `submitter` (a submit button, or null) does; nothing if
// the page can't be submitted this way (a dialog method, an unusable address).
[[nodiscard]] std::optional<FormSubmission> submitForm(const Node* form, const Node* submitter, const FormState& state,
                                                       const std::string& document_url);
// The submit button Enter in a field uses: the form's first, if any.
[[nodiscard]] const Node* defaultButton(const Node* form);
// Whether Enter in a field may submit a form without a submit button: only when it
// has a single field that takes typing (the spec's rule).
[[nodiscard]] bool implicitSubmissionAllowed(const Node* form);

// application/x-www-form-urlencoded, as the URL standard writes it.
[[nodiscard]] std::string urlEncodeForm(const std::vector<std::pair<std::string, std::string>>& entries);

// Limits a page can't push past.
inline constexpr size_t kMaxFormEntries = 10'000;
inline constexpr size_t kMaxFieldBytes = 1 << 20;

}  // namespace fbx::xweb
