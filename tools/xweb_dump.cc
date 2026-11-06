// xweb_dump file.html — the document we parse from it, serialized.

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xweb/html_parser.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: xweb_dump file.html\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "can't read " << argv[1] << "\n";
        return 1;
    }
    const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::cout << fbx::xweb::serializeDocument(*fbx::xweb::parseHtml(html));
    return 0;
}
