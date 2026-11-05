#include "render/page_refresh_regions.h"
#include "check.h"

#include <QImage>
#include <QRect>
#include <QRegion>
#include <cstdio>

int main()
{
    require(!kobalt::hasVisibleColor(qRgb(100, 110, 100)), "Near-gray pixel counted as colour");
    require(kobalt::hasVisibleColor(qRgb(100, 111, 100)),
            "Colour threshold excluded a visible tint");
    QImage page(100, 100, QImage::Format_RGB32);
    page.setText("kobalt.image_regions", "[[10,10,60,80]]");
    require(kobalt::pageImageAtPoint(page, { 40, 50 }), "Cover was not an image target");
    require(!kobalt::pageImageAtPoint(page, { 0, 0 }), "Paper outside the image became a target");
    require(kobalt::pageSingleImage(page).isEmpty(), "Unknown text metadata offered image zoom");
    page.setText("kobalt.image_only", "true");
    require(kobalt::pageSingleImage(page) == QRect(10, 10, 60, 80),
            "Image-only cover lost zoom suggestion");
    page.setText("kobalt.image_only", "false");
    require(kobalt::pageSingleImage(page).isEmpty(), "Image with text offered zoom suggestion");
    page.setText("kobalt.image_only", "true");
    page.setText("kobalt.image_regions", "[[10,10,60,80],[0,0,2,2]]");
    require(kobalt::pageSingleImage(page).isEmpty(), "Multiple images offered zoom suggestion");
    page.setText("kobalt.image_regions", "[[10,10,10,10],[0,50,100,5]]");
    require(kobalt::pageSingleImage(page).isEmpty(), "Small images offered zoom suggestion");
    require(!kobalt::pageImageAtPoint(page, { 15, 15 }), "Small ornament became an image target");
    require(!kobalt::pageImageAtPoint(page, { 50, 52 }), "Narrow wordmark became an image target");
    page.setText("kobalt.image_regions", "malformed");
    require(!kobalt::pageImageAtPoint(page, { 40, 50 }), "Invalid metadata became an image target");
    page.setText("kobalt.image_regions", QString());
    const QRegion full(QRect(0, 0, 100, 100));

    page.setText("kobalt.animation_text_only", "false");
    require(kobalt::pageRefreshRegions(page, false) == full,
            "Missing metadata did not use the old full-page fallback");
    page.setText("kobalt.animation_text_only", "true");
    require(kobalt::pageRefreshRegions(page, true).isEmpty(),
            "Missing metadata did not use the old text fallback");

    page.setText("kobalt.image_regions", "[]");
    page.setText("kobalt.colour_regions", "[]");
    page.setText("kobalt.animation_text_only", "false");
    require(kobalt::pageRefreshRegions(page, true).isEmpty(),
            "Explicit empty regions used the legacy fallback");

    page.setText("kobalt.image_regions", "[[0,0,20,20],[40,0,40,25],[60,60,40,40]]");
    require(kobalt::pageRefreshRegions(page, false)
                    == QRegion(QRect(40, 0, 40, 25)) + QRect(60, 60, 40, 40),
            "Large image regions were lost or small images were included");

    page.setText("kobalt.image_regions", "[[-20,0,30,100],[90,80,50,20]]");
    require(kobalt::pageRefreshRegions(page, false) == QRegion(QRect(0, 0, 10, 100)),
            "Image area was measured before clipping to the page");

    page.setText("kobalt.image_regions", "[]");
    page.setText("kobalt.colour_regions", "[[0,0,32,32],[64,64,32,32]]");
    require(kobalt::pageRefreshRegions(page, true)
                    == QRegion(QRect(0, 0, 32, 32)) + QRect(64, 64, 32, 32),
            "Colour text regions were not included");
    require(kobalt::pageRefreshRegions(page, false).isEmpty(),
            "Colour regions were used on a monochrome device");

    page.setText("kobalt.image_regions", "[[0,0,10,1.5]]");
    require(kobalt::pageRefreshRegions(page, false) == full,
            "Fractional metadata did not use the old fallback");
    page.setText("kobalt.animation_text_only", "true");
    require(kobalt::pageRefreshRegions(page, false).isEmpty(),
            "Malformed metadata ignored the old text fallback");
    page.setText("kobalt.image_regions", "[]");
    page.setText("kobalt.colour_regions", "not-json");
    page.setText("kobalt.animation_text_only", "false");
    require(kobalt::pageRefreshRegions(page, true) == full,
            "Malformed colour metadata did not use the old fallback");

    std::puts(
            "PASS: page refresh regions use clipped image and colour metadata with safe fallback");
}
