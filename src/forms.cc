// Form controls' state and form submission, after the HTML standard's "constructing
// the entry list" and "form submission algorithm", less what needs scripts or files.

#include "xweb/forms.h"

#include <algorithm>
#include <cctype>
#include <random>

#include "xweb/encoding.h"
#include "xweb/url.h"

namespace fbx::xweb {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string attrOr(const Node* n, std::string_view name, std::string fallback = {}) {
    const std::string* v = n->attr(name);
    return v ? *v : fallback;
}

const Node* topOf(const Node* n) {
    while (n->parent) n = n->parent;
    return n;
}

// Every element in tree order, through templates' contents too no: they aren't rendered.
template <typename F>
void eachElement(const Node* root, F f) {
    std::vector<const Node*> todo{root};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (n->isElement() && !f(n)) return;
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
}

const Node* byId(const Node* root, const std::string& id) {
    const Node* found = nullptr;
    eachElement(root, [&](const Node* n) {
        const std::string* v = n->attr("id");
        if (v && *v == id) {
            found = n;
            return false;
        }
        return true;
    });
    return found;
}

std::string collapse(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string crlf(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += "\r\n";
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else if (s[i] == '\n') {
            out += "\r\n";
        } else {
            out += s[i];
        }
    }
    return out;
}

bool isListed(const Node* n) {
    return n->isHtml("input") || n->isHtml("select") || n->isHtml("textarea") || n->isHtml("button");
}

std::vector<const Node*> controlsOf(const Node* form) {
    std::vector<const Node*> out;
    eachElement(topOf(form), [&](const Node* n) {
        if (isListed(n) && formOwner(n) == form) out.push_back(n);
        return out.size() < kMaxFormEntries;
    });
    return out;
}

bool isSubmitButton(ControlKind k) { return k == ControlKind::Submit || k == ControlKind::Image; }

}  // namespace

ControlKind controlKind(const Node* n) {
    if (!n || !n->isElement() || n->ns != Namespace::Html) return ControlKind::None;
    if (n->name == "textarea") return ControlKind::TextArea;
    if (n->name == "select") return ControlKind::Select;
    if (n->name == "button") {
        const std::string t = lower(attrOr(n, "type"));
        if (t == "reset") return ControlKind::Reset;
        if (t == "button") return ControlKind::Button;
        return ControlKind::Submit;
    }
    if (n->name != "input") return ControlKind::None;
    const std::string t = lower(attrOr(n, "type"));
    if (t == "password") return ControlKind::Password;
    if (t == "checkbox") return ControlKind::Checkbox;
    if (t == "radio") return ControlKind::Radio;
    if (t == "submit") return ControlKind::Submit;
    if (t == "image") return ControlKind::Image;
    if (t == "reset") return ControlKind::Reset;
    if (t == "button" || t == "range" || t == "color") return ControlKind::Button;
    if (t == "hidden") return ControlKind::Hidden;
    if (t == "file") return ControlKind::File;
    return ControlKind::Text;
}

bool isDisabled(const Node* control) {
    if (control->attr("disabled")) return true;
    const Node* child = control;
    for (const Node* p = control->parent; p; child = p, p = p->parent) {
        if (!p->isHtml("fieldset") || !p->attr("disabled")) continue;
        // Its first legend's contents stay usable.
        const Node* first_legend = nullptr;
        for (const Node* c : p->children)
            if (c->isHtml("legend")) {
                first_legend = c;
                break;
            }
        if (child != first_legend) return true;
    }
    return false;
}

const Node* formOwner(const Node* control) {
    if (!control) return nullptr;
    if (const std::string* id = control->attr("form")) {
        const Node* f = byId(topOf(control), *id);
        return f && f->isHtml("form") ? f : nullptr;
    }
    for (const Node* p = control->parent; p; p = p->parent)
        if (p->isHtml("form")) return p;
    return nullptr;
}

std::vector<const Node*> optionsOf(const Node* select) {
    std::vector<const Node*> out;
    if (!select) return out;
    for (const Node* c : select->children) {
        if (c->isHtml("option")) out.push_back(c);
        else if (c->isHtml("optgroup"))
            for (const Node* o : c->children)
                if (o->isHtml("option")) out.push_back(o);
        if (out.size() >= kMaxFormEntries) break;
    }
    return out;
}

const Node* labelledControl(const Node* label) {
    if (!label || !label->isHtml("label")) return nullptr;
    if (const std::string* f = label->attr("for")) {
        const Node* target = byId(topOf(label), *f);
        return target && controlKind(target) != ControlKind::None ? target : nullptr;
    }
    const Node* found = nullptr;
    eachElement(label, [&](const Node* n) {
        if (n != label && controlKind(n) != ControlKind::None && controlKind(n) != ControlKind::Hidden) {
            found = n;
            return false;
        }
        return true;
    });
    return found;
}

std::string optionValue(const Node* option) {
    if (const std::string* v = option->attr("value")) return *v;
    return collapse(option->textContent());
}

std::string optionLabel(const Node* option) {
    if (const std::string* v = option->attr("label"); v && !v->empty()) return *v;
    return collapse(option->textContent());
}

// ── State ─────────────────────────────────────────────────────────────────────

std::string FormState::value(const Node* control) const {
    if (const auto it = states_.find(control); it != states_.end() && it->second.has_value) return it->second.value;
    if (controlKind(control) == ControlKind::TextArea) {
        std::string t = control->textContent();
        return t.size() > kMaxFieldBytes ? t.substr(0, kMaxFieldBytes) : t;
    }
    return attrOr(control, "value");
}

bool FormState::checked(const Node* control) const {
    if (const auto it = states_.find(control); it != states_.end() && it->second.has_checked) return it->second.checked;
    return control->attr("checked") != nullptr;
}

int FormState::selected(const Node* select) const {
    const std::vector<const Node*> options = optionsOf(select);
    if (const auto it = states_.find(select); it != states_.end() && it->second.selected != -2)
        return it->second.selected < static_cast<int>(options.size()) ? it->second.selected : -1;
    int pick = -1;
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i]->attr("selected")) pick = static_cast<int>(i);
    if (pick < 0 && !select->attr("multiple")) {
        // A drop-down always shows something: the first option that isn't disabled.
        for (size_t i = 0; i < options.size(); ++i)
            if (!options[i]->attr("disabled")) return static_cast<int>(i);
    }
    return pick;
}

void FormState::setValue(const Node* control, std::string value) {
    if (value.size() > kMaxFieldBytes) value.resize(kMaxFieldBytes);
    if (controlKind(control) != ControlKind::TextArea) value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return c == '\n' || c == '\r'; }), value.end());
    // An input's maxlength, in characters.
    if (const std::string* ml = control->attr("maxlength")) {
        char* end = nullptr;
        const long n = std::strtol(ml->c_str(), &end, 10);
        if (end != ml->c_str() && n >= 0) {
            size_t chars = 0, i = 0;
            for (; i < value.size(); ++i)
                if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80 && chars++ == static_cast<size_t>(n)) break;
            value.resize(i);
        }
    }
    State& s = states_[control];
    s.has_value = true;
    s.value = std::move(value);
}

void FormState::activate(const Node* control) {
    const ControlKind k = controlKind(control);
    if (k == ControlKind::Checkbox) {
        const bool now = !checked(control);
        State& s = states_[control];
        s.has_checked = true;
        s.checked = now;
        return;
    }
    if (k != ControlKind::Radio) return;
    const std::string name = attrOr(control, "name");
    const Node* form = formOwner(control);
    if (!name.empty()) {
        eachElement(topOf(control), [&](const Node* n) {
            if (n != control && controlKind(n) == ControlKind::Radio && attrOr(n, "name") == name && formOwner(n) == form) {
                State& o = states_[n];
                o.has_checked = true;
                o.checked = false;
            }
            return true;
        });
    }
    State& s = states_[control];
    s.has_checked = true;
    s.checked = true;
}

void FormState::setChecked(const Node* control, bool on) {
    if (on && controlKind(control) == ControlKind::Radio) {
        if (!checked(control)) activate(control);
        return;
    }
    State& st = states_[control];
    st.has_checked = true;
    st.checked = on;
}

void FormState::select(const Node* select, int index) {
    const int count = static_cast<int>(optionsOf(select).size());
    states_[select].selected = index >= 0 && index < count ? index : -1;
}

void FormState::reset(const Node* form) {
    for (const Node* c : controlsOf(form)) states_.erase(c);
    if (focused && formOwner(focused) == form) caret = value(focused).size();
}

// ── Submission ────────────────────────────────────────────────────────────────

std::string urlEncodeForm(const std::vector<std::pair<std::string, std::string>>& entries) {
    static const char* hex = "0123456789ABCDEF";
    const auto encode = [&](const std::string& s, std::string& out) {
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') out += static_cast<char>(c);
            else if (c == ' ') out += '+';
            else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
    };
    std::string out;
    for (const auto& [name, value] : entries) {
        if (!out.empty()) out += '&';
        encode(name, out);
        out += '=';
        encode(value, out);
    }
    return out;
}

const Node* defaultButton(const Node* form) {
    for (const Node* c : controlsOf(form))
        if (isSubmitButton(controlKind(c))) return c;
    return nullptr;
}

bool implicitSubmissionAllowed(const Node* form) {
    int fields = 0;
    for (const Node* c : controlsOf(form)) {
        const ControlKind k = controlKind(c);
        if (k == ControlKind::Text || k == ControlKind::Password) ++fields;
    }
    return fields == 1;
}

std::optional<FormSubmission> submitForm(const Node* form, const Node* submitter, const FormState& state, const std::string& document_url,
                                         const std::string& charset) {
    if (!form || !form->isHtml("form")) return std::nullopt;
    const auto setting = [&](std::string_view on_button, std::string_view on_form) {
        if (submitter && controlKind(submitter) != ControlKind::None)
            if (const std::string* v = submitter->attr(on_button)) return *v;
        return attrOr(form, on_form);
    };
    const std::string method = lower(setting("formmethod", "method"));
    if (method == "dialog") return std::nullopt;
    const bool post = method == "post";
    std::string enctype = lower(setting("formenctype", "enctype"));
    if (enctype != "multipart/form-data" && enctype != "text/plain") enctype = "application/x-www-form-urlencoded";
    const std::string action_attr = setting("formaction", "action");
    std::string action = action_attr.empty() ? document_url : resolveUrl(document_url, action_attr);
    const Url target = parseUrl(action);
    if (target.scheme != "http" && target.scheme != "https" && target.scheme != "file") return std::nullopt;

    // In the encoding the form asks for (accept-charset's first we know) or else the page's:
    // a search on an EUC-KR site (yes24) expects EUC-KR.
    bool euc_kr = isEucKrLabel(charset);
    if (const std::string* accept = form->attr("accept-charset")) {
        std::string word;
        for (size_t i = 0; i <= accept->size(); ++i) {
            if (i == accept->size() || (*accept)[i] == ' ' || (*accept)[i] == ',') {
                if (lower(word) == "utf-8") { euc_kr = false; break; }
                if (isEucKrLabel(word)) { euc_kr = true; break; }
                word.clear();
            } else {
                word += (*accept)[i];
            }
        }
    }
    // The entry list.
    std::vector<std::pair<std::string, std::string>> entries;
    size_t bytes = 0;
    const auto add = [&](std::string name, std::string value) {
        if (entries.size() >= kMaxFormEntries || bytes > 16 * kMaxFieldBytes) return;
        bytes += name.size() + value.size();
        entries.emplace_back(crlf(name), crlf(value));
    };
    for (const Node* c : controlsOf(form)) {
        const ControlKind k = controlKind(c);
        if (isDisabled(c)) continue;
        if ((k == ControlKind::Submit || k == ControlKind::Image || k == ControlKind::Reset || k == ControlKind::Button) && c != submitter) continue;
        if (k == ControlKind::Reset || k == ControlKind::Button || k == ControlKind::File) continue;
        if ((k == ControlKind::Checkbox || k == ControlKind::Radio) && !state.checked(c)) continue;
        const std::string name = attrOr(c, "name");
        if (k == ControlKind::Image) {
            // Where it was clicked: we don't say, so the top left.
            const std::string prefix = name.empty() ? "" : name + ".";
            add(prefix + "x", "0");
            add(prefix + "y", "0");
            continue;
        }
        if (name.empty()) continue;
        if (k == ControlKind::Select) {
            const std::vector<const Node*> options = optionsOf(c);
            if (c->attr("multiple")) {
                for (const Node* o : options)
                    if (o->attr("selected") && !o->attr("disabled")) add(name, optionValue(o));
            } else if (const int i = state.selected(c); i >= 0 && !options[static_cast<size_t>(i)]->attr("disabled")) {
                add(name, optionValue(options[static_cast<size_t>(i)]));
            }
            continue;
        }
        if (k == ControlKind::Checkbox || k == ControlKind::Radio) {
            add(name, attrOr(c, "value", "on"));
            continue;
        }
        if (k == ControlKind::Hidden && lower(name) == "_charset_") {
            add(name, euc_kr ? "EUC-KR" : "UTF-8");
            continue;
        }
        add(name, state.value(c));
    }

    if (euc_kr && enctype != "multipart/form-data")
        for (auto& [n, v] : entries) {
            n = utf8ToEucKr(n);
            v = utf8ToEucKr(v);
        }
    FormSubmission out;
    out.post = post && target.scheme != "file";
    if (!post) {
        Url u = target;
        u.query = urlEncodeForm(entries);
        u.has_query = true;
        out.url = u.toString();
        return out;
    }
    out.url = action;
    if (enctype == "application/x-www-form-urlencoded") {
        out.content_type = enctype;
        out.body = urlEncodeForm(entries);
    } else if (enctype == "text/plain") {
        out.content_type = "text/plain;charset=UTF-8";
        for (const auto& [n, v] : entries) out.body += n + "=" + v + "\r\n";
    } else {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        static const char* hex = "0123456789abcdef";
        std::string boundary = "----xwebFormBoundary";
        for (int i = 0; i < 16; ++i) boundary += hex[rng() & 15];
        out.content_type = "multipart/form-data; boundary=" + boundary;
        const auto quoted = [](const std::string& s) {
            std::string q;
            for (char c : s) {
                if (c == '"') q += "%22";
                else if (c == '\r') q += "%0D";
                else if (c == '\n') q += "%0A";
                else q += c;
            }
            return q;
        };
        for (const auto& [n, v] : entries)
            out.body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + quoted(n) + "\"\r\n\r\n" + v + "\r\n";
        out.body += "--" + boundary + "--\r\n";
    }
    return out;
}

}  // namespace fbx::xweb
