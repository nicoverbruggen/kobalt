#ifndef LVFNTMAN_VERT_BUFFER_H_INCLUDED
#define LVFNTMAN_VERT_BUFFER_H_INCLUDED

#include <cstddef>
#include <cstring>
#include <limits>

// Preserve rows when a rotated horizontal run outgrows its measured width.
inline void ensureVerticalRotationBufferWidth(unsigned char *&buffer, int &width,
                                              int height, int needed_width)
{
    if (needed_width <= width)
        return;
    int expanded_width = needed_width;
    if (width <= std::numeric_limits<int>::max() / 2 && width * 2 > expanded_width)
        expanded_width = width * 2;
    unsigned char *expanded = new unsigned char[(std::size_t)expanded_width * height]();
    for (int row = 0; row < height; row++)
        std::memcpy(expanded + (std::size_t)row * expanded_width,
                    buffer + (std::size_t)row * width, width);
    delete[] buffer;
    buffer = expanded;
    width = expanded_width;
}

#endif // LVFNTMAN_VERT_BUFFER_H_INCLUDED
