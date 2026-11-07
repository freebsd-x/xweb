// xjs file.js... — runs scripts on our JavaScript engine, console output to stdout and
// stderr. Exit status 1 if a script threw or failed to parse.

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xweb/js.h"

int main(int argc, char** argv) {
    using namespace fbx::xweb::js;
    Runtime rt;
    rt.setConsole([](std::string_view level, const std::string& text) {
        (level == "error" || level == "warn" ? std::cerr : std::cout) << text << "\n";
    });
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::cerr << "can't read " << argv[i] << "\n";
            return 2;
        }
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const Runtime::Result r = rt.evaluate(src, argv[i]);
        if (!r.ok) {
            std::cerr << argv[i] << ": " << r.error << "\n";
            status = 1;
        }
        rt.collectGarbage();
    }
    return status;
}
