// Runs the html5lib tree-construction suite (.dat files) against the parser.
//   xweb_html5lib_test <dir> <known-failures-file> [-v]
// A test in the known-failures file ("file.dat:index") may fail; any other failure
// fails the run, and so does a listed test that now passes, so the list stays honest.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xweb/html_parser.h"

using namespace fbx::xweb;

namespace {

std::string displayAttr(const Node& el, const std::string& name) {
    if (el.ns == Namespace::Html) return name;
    static const std::map<std::string, std::string> foreign = {
        {"xlink:actuate", "xlink actuate"}, {"xlink:arcrole", "xlink arcrole"}, {"xlink:href", "xlink href"},
        {"xlink:role", "xlink role"},       {"xlink:show", "xlink show"},       {"xlink:title", "xlink title"},
        {"xlink:type", "xlink type"},       {"xml:lang", "xml lang"},           {"xml:space", "xml space"},
        {"xmlns", "xmlns"},                 {"xmlns:xlink", "xmlns xlink"},
    };
    const auto it = foreign.find(name);
    return it == foreign.end() ? name : it->second;
}

void dump(const Node& n, int depth, std::string& out) {
    const std::string pad = "| " + std::string(static_cast<size_t>(depth) * 2, ' ');
    switch (n.type) {
        case NodeType::Doctype:
            out += pad + "<!DOCTYPE " + n.name;
            if (!n.public_id.empty() || !n.system_id.empty())
                out += " \"" + n.public_id + "\" \"" + n.system_id + "\"";
            out += ">\n";
            break;
        case NodeType::Comment:
            out += pad + "<!-- " + n.data + " -->\n";
            break;
        case NodeType::Text:
            out += pad + "\"" + n.data + "\"\n";
            break;
        case NodeType::Element: {
            out += pad + "<";
            if (n.ns == Namespace::Svg) out += "svg ";
            if (n.ns == Namespace::MathMl) out += "math ";
            out += n.name + ">\n";
            std::vector<std::pair<std::string, std::string>> attrs;
            for (const auto& a : n.attrs) attrs.emplace_back(displayAttr(n, a.name), a.value);
            std::sort(attrs.begin(), attrs.end());
            for (const auto& [k, v] : attrs) out += pad + "  " + k + "=\"" + v + "\"\n";
            if (n.template_content) {
                out += pad + "  content\n";
                for (const Node* c : n.template_content->children) dump(*c, depth + 2, out);
            }
            for (const Node* c : n.children) dump(*c, depth + 1, out);
            break;
        }
        default:
            break;
    }
}

struct Case {
    std::string data, expected, fragment;
    bool script_on = false;
};

std::vector<Case> readCases(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<Case> cases;
    std::string line, section;
    Case c;
    bool started = false;
    const auto finish = [&] {
        if (!c.data.empty() && c.data.back() == '\n') c.data.pop_back();
        while (c.expected.size() >= 2 && c.expected.ends_with("\n\n")) c.expected.pop_back();
        if (!c.expected.empty() && c.expected.back() != '\n') c.expected += '\n';
        cases.push_back(std::move(c));
        c = {};
    };
    while (std::getline(in, line)) {
        if (line == "#data") {
            if (started) finish();
            started = true;
            section = "data";
            continue;
        }
        if (line == "#errors" || line == "#new-errors" || line == "#document" || line == "#document-fragment") {
            section = line.substr(1);
            continue;
        }
        if (line == "#script-on") { c.script_on = true; section.clear(); continue; }
        if (line == "#script-off") { section.clear(); continue; }
        if (section == "data") c.data += line + "\n";
        else if (section == "document") c.expected += line + "\n";
        else if (section == "document-fragment") c.fragment = line;
    }
    if (started) finish();
    return cases;
}

std::string run(const Case& c) {
    ParseOptions options;
    options.scripting = c.script_on;
    std::string out;
    if (c.fragment.empty()) {
        const auto doc = parseHtml(c.data, options);
        for (const Node* n : doc->root()->children) dump(*n, 0, out);
        return out;
    }
    Namespace ns = Namespace::Html;
    std::string context = c.fragment;
    if (context.starts_with("svg ")) { ns = Namespace::Svg; context = context.substr(4); }
    else if (context.starts_with("math ")) { ns = Namespace::MathMl; context = context.substr(5); }
    const auto doc = parseFragment(c.data, context, ns, options);
    for (const Node* n : doc->documentElement()->children) dump(*n, 0, out);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: xweb_html5lib_test <dir> <known-failures> [-v]\n";
        return 2;
    }
    const bool verbose = argc > 3 && std::string(argv[3]) == "-v";
    std::set<std::string> known;
    {
        std::ifstream in(argv[2]);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty() && line[0] != '#') known.insert(line.substr(0, line.find(' ')));
    }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        if (e.path().extension() == ".dat") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    int total = 0, passed = 0, unexpected = 0, stale = 0;
    std::map<std::string, std::pair<int, int>> per_file;
    for (const auto& file : files) {
        const auto cases = readCases(file);
        for (size_t i = 0; i < cases.size(); ++i) {
            const std::string id = file.filename().string() + ":" + std::to_string(i);
            const std::string got = run(cases[i]);
            const bool ok = got == cases[i].expected;
            ++total;
            ++per_file[file.filename().string()].second;
            if (ok) {
                ++passed;
                ++per_file[file.filename().string()].first;
                if (known.count(id)) {
                    ++stale;
                    std::cout << "NOW PASSES (remove from the list): " << id << "\n";
                }
                continue;
            }
            if (known.count(id)) continue;
            ++unexpected;
            std::cout << "FAIL " << id << "\n";
            if (verbose)
                std::cout << "--- input" << (cases[i].fragment.empty() ? "" : " in " + cases[i].fragment) << "\n"
                          << cases[i].data << "\n--- expected\n" << cases[i].expected << "--- got\n" << got << "\n";
        }
    }
    for (const auto& [name, counts] : per_file)
        if (counts.first != counts.second)
            std::cout << "  " << name << ": " << counts.first << "/" << counts.second << "\n";
    std::cout << passed << " of " << total << " pass; " << unexpected << " unexpected failures\n";
    return unexpected || stale ? 1 : 0;
}
