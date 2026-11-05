#pragma once
#include "assets/assets.h"
#include "assets/bundled_fonts.h"

namespace kobalt::test {
inline AssetBundle &readerAssets()
{
    static AssetBundle assets(QStringLiteral(KOBALT_TEST_ASSET_ROOT),
                              QStringLiteral(KOBALT_VERSION));
    return assets;
}

inline QString alternateReadingFace()
{
    return bundledFontFiles(readerAssets())
                    .contains(readerAssets().file("fonts/ebook-core/NV_Ancizar_Serif-Regular.ttf"))
            ? QStringLiteral("NV Ancizar Serif")
            : QStringLiteral("NV Bitter");
}

inline QString alternateReadingLabel()
{
    return alternateReadingFace() == QStringLiteral("NV Bitter") ? QStringLiteral("Bitter")
                                                                 : QStringLiteral("Ancizar Serif");
}

inline QString headingReadingFace()
{
    return bundledFontFiles(readerAssets())
                    .contains(readerAssets().file("fonts/ebook-core/NV_Legible_Next-Regular.ttf"))
            ? QStringLiteral("NV Legible Next")
            : QStringLiteral("NV Bitter");
}

inline FontCopies &readerFontCopies()
{
    static FontCopies copies;
    return copies;
}

inline EngineFontInputs readerFonts()
{
    // The staged fixture is immutable for this process. Reuse its verified
    // paths instead of checking the full bundle for every model in a matrix.
    // Tests of asset changes use their own bundles and production verification.
#ifdef KOBALT_TEST_VARIABLE_FONT_DIRECTORY
    static const auto inputs = engineFontInputs(
            readerAssets(), {}, QStringLiteral(KOBALT_TEST_VARIABLE_FONT_DIRECTORY));
#else
    static const auto inputs = engineFontInputs(readerAssets(), {}, {});
#endif
    return inputs;
}

inline QString readerPlaceholder()
{
    static const QString family = placeholderFontFamily(readerAssets(), readerFontCopies());
    return family;
}
} // namespace kobalt::test
