#include "check.h"

#include <lunasvg.h>

#include <string>

static std::string nestedSvg(unsigned groups) {
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>";
    for (unsigned i = 0; i < groups; ++i)
        svg += "<g>";
    svg += "<rect width='16' height='16' fill='red'/>";
    for (unsigned i = 0; i < groups; ++i)
        svg += "</g>";
    return svg + "</svg>";
}

static bool paintsReferenceChain(unsigned references) {
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><defs>";
    svg += "<g id='n0'><rect width='16' height='16' fill='red'/></g>";
    for (unsigned i = 1; i < references; ++i)
        svg += "<g id='n" + std::to_string(i) + "'><use href='#n" + std::to_string(i - 1) + "'/></g>";
    svg += "</defs><use href='#n" + std::to_string(references - 1) + "'/></svg>";
    auto document = lunasvg::Document::loadFromData(svg);
    require(document != nullptr, "Reference chain rejected during parsing");
    auto bitmap = document->renderToBitmap(16, 16);
    require(bitmap.data() != nullptr, "Reference chain bitmap missing");
    bitmap.convertToRGBA();
    return bitmap.data()[3] != 0;
}

int main() {
    auto allowed = lunasvg::Document::loadFromData(nestedSvg(62));
    require(allowed != nullptr, "SVG at the nesting limit rejected");
    require(allowed->renderToBitmap(16, 16).data() != nullptr, "Nested SVG did not render");
    require(lunasvg::Document::loadFromData(nestedSvg(63)) == nullptr, "SVG nesting limit not enforced");

    std::string siblings = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>";
    for (unsigned i = 0; i < 100; ++i)
        siblings += "<g><rect width='1' height='1'/></g>";
    require(lunasvg::Document::loadFromData(siblings + "</svg>") != nullptr,
            "Sibling elements accumulated nesting depth");

    require(paintsReferenceChain(16), "SVG at the reference limit did not render");
    require(!paintsReferenceChain(17), "SVG reference limit not enforced");
}
