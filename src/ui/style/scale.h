#pragma once
#include <QMargins>
#include <QtGlobal>

namespace kobalt::ui {
// Kobalt's UI is designed in pixels of a 300 ppi screen, the density of the
// Clara and Libra. Every size scales to the device's density, so a control has
// the same physical size on every screen.
inline constexpr int designPpi = 300;
namespace detail {
inline int ppi = designPpi;
inline qreal textScale = 1;
inline qreal iconScale = 1;
}
// Set before any view is built. Views read sizes when they lay out, so a view
// built earlier keeps the sizes of the old density.
inline void setDensity(int ppi) {
    detail::ppi = ppi > 0 ? ppi : designPpi;
}
inline int density() {
    return detail::ppi;
}
// A design size in device pixels. A size that is not zero stays at least one
// pixel, so a rule never disappears.
inline int px(int design) {
    if (!design || detail::ppi == designPpi)
        return design;
    const int scaled = qRound(design * qreal(detail::ppi) / designPpi);
    return scaled ? scaled : (design > 0 ? 1 : -1);
}

// Large text enlarges interface text by this factor on top of the density.
// Controls, icons and spacing keep their size; rows that hold text grow with
// it. Set before any view is built, as the density is.
inline void setTextScale(qreal scale) {
    detail::textScale = scale > 0 ? scale : 1;
}
inline qreal textScale() {
    return detail::textScale;
}
// Large text can also enlarge the icons that sit in cells, which keep their size.
inline void setIconScale(qreal scale) {
    detail::iconScale = scale > 0 ? scale : 1;
}
// An icon size in device pixels.
inline int iconPx(int design) {
    const int pixels = px(design);
    return detail::iconScale == 1 ? pixels : qRound(pixels * detail::iconScale);
}
// The size of an icon in a cell, which large text can enlarge.
class IconPx {
public:
    constexpr explicit IconPx(int design) : value(design) {}
    operator int() const {
        return iconPx(value);
    }
    constexpr int design() const {
        return value;
    }

private:
    int value;
};
// Applies a platform's text size setting. Text grows up to 150%: controls
// keep their size, and larger text no longer fits their fixed cells and
// caption columns. Icons in cells grow less, to 120% at the largest step.
inline void setLargeText(qreal requested) {
    const qreal text = qBound<qreal>(1, requested, 1.5);
    setTextScale(text);
    setIconScale(1 + (text - 1) * 0.4);
}
// A text size in device pixels.
inline int textPx(int design) {
    const int pixels = px(design);
    return detail::textScale == 1 ? pixels : qRound(pixels * detail::textScale);
}
// A font size in design pixels, which large text enlarges.
class TextPx {
public:
    constexpr explicit TextPx(int design) : value(design) {}
    operator int() const {
        return textPx(value);
    }
    constexpr int design() const {
        return value;
    }

private:
    int value;
};

// A metric in design pixels. It converts to device pixels where it is used. A
// sum of metrics scales each part and then adds them, so edges that add up on
// a 300 ppi screen still meet after rounding.
class Px {
public:
    constexpr explicit Px(int design) : parts{design, 0, 0, 0} {}
    operator int() const {
        return px(parts[0]) + px(parts[1]) + px(parts[2]) + px(parts[3]);
    }
    // The value on a 300 ppi screen.
    constexpr int design() const {
        return parts[0] + parts[1] + parts[2] + parts[3];
    }
    friend constexpr Px operator+(Px a, Px b) {
        Px sum = a;
        int next = 0;
        while (next < 4 && sum.parts[next])
            ++next;
        for (int part : b.parts)
            if (part)
                sum.parts[next++] = part;
        return sum;
    }

private:
    int parts[4];
};

class PxMargins {
public:
    constexpr PxMargins(Px left, Px top, Px right, Px bottom) : l(left), t(top), r(right), b(bottom) {}
    constexpr PxMargins(int left, int top, int right, int bottom) : l(left), t(top), r(right), b(bottom) {}
    constexpr Px left() const {
        return l;
    }
    constexpr Px top() const {
        return t;
    }
    constexpr Px right() const {
        return r;
    }
    constexpr Px bottom() const {
        return b;
    }
    operator QMargins() const {
        return {l, t, r, b};
    }

private:
    Px l, t, r, b;
};
}
