#include "platforms/kobo/display/image_format.h"
#include <cstdio>
#include <cstdlib>

static void require(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

int main()
{
    using namespace kobalt;
    require(needsPremultipliedDisplay(QImage::Format_RGBA8888),
            "straight RGBA needs the workaround");
    for (auto format : { QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBX8888,
                         QImage::Format_RGB16, QImage::Format_RGB32, QImage::Format_Invalid })
        require(!needsPremultipliedDisplay(format),
                "fixed or unsupported formats must be untouched");
    require(displayFormatPaintCheck(), "premultiplied drawing changed the rendered colours");
    bool eligible = false;
    {
        DisplayAllocationScope reader(eligible, true);
        require(eligible, "reader allocation scope");
        {
            DisplayAllocationScope stock(eligible, false);
            require(!eligible, "nested stock allocation must not inherit reader eligibility");
        }
        require(eligible, "nested scope must restore reader eligibility");
    }
    require(!eligible, "reader eligibility must end with its native call");
    return 0;
}
