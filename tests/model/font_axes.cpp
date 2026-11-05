#include "assets/font_catalog.h"
#include "check.h"

int main()
{
    // Restore the axis assertions from the old reader regression without
    // requiring an external variable-font download for the bounds check.
    const QVector<kobalt::ReadingFont> fonts{ { "Variable fixture",
                                                {},
                                                kobalt::FontOrigin::Sideloaded,
                                                { 300, 650, 450 },
                                                { 6.25, 72.75, 14 } } };
    require(kobalt::fontWeightAxis(fonts, "VARIABLE FIXTURE").defaultValue == 450
                    && !kobalt::fontWeightAxis(fonts, "Missing font").available(),
            "Weight axis lookup lost family matching");
    const auto optical = kobalt::fontOpticalSizeAxis(fonts, "Variable fixture");
    require(kobalt::normalizedOpticalSize(optical, 0) == 14
                    && kobalt::normalizedOpticalSize(optical, 6) == 7
                    && kobalt::normalizedOpticalSize(optical, 73) == 72
                    && kobalt::normalizedOpticalSize(optical, 14.6) == 15,
            "Optical size normalization lost defaults or whole-point bounds");
}
