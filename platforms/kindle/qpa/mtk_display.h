#pragma once
#include "refresh_queue.h"
#include <cstddef>
#include <cstdint>

namespace kindle::mtk {
// Linux userspace ABI, independently declared from Amazon's 5.19.2 source
// release (include/linux/hwtcon_ioctl_cmd.h). All fields are 32 bits on ARM.
// https://s3.amazonaws.com/kindledownloads/Kindle_src_5.19.2_4670760040.tar.gz
struct Rect { uint32_t top, left, width, height; };
struct Update {
    Rect region;
    uint32_t waveform, mode, marker;
    int32_t temperature;
    uint32_t flags;
    int32_t dither, quantization;
    uint32_t alternateAddress, alternateWidth, alternateHeight;
    Rect alternateRegion;
    uint32_t swipeDirection, swipeSteps;
    uint32_t histogramBlackWhite, histogramGray, timestampPxp, timestampEpdc;
};
struct Completion { uint32_t marker, collision; };
static_assert(sizeof(Update) == 96 && offsetof(Update, swipeDirection) == 72 &&
              offsetof(Update, histogramBlackWhite) == 80);
static_assert(sizeof(Completion) == 8);
constexpr unsigned long sendUpdate = 0x4060462e;
constexpr unsigned long waitComplete = 0xc008462f;

struct Layout {
    int width = 0, height = 0;
    size_t stride = 0, bytes = 0;
    bool inverted = false;
};
bool validLayout(const Layout &layout);
QImage initialFrame(const QSize &size);
bool copyPixels(unsigned char *destination, const Layout &layout, const QImage &pixels, const QRect &area);
Update makeUpdate(const QRect &area, bool full, uint32_t marker, int direction, bool swipeAvailable);
bool supportsSwipe(const QByteArray &header, const QByteArray &loadedName);

// One worker owns present(). Destruction follows RefreshQueue::stop(), so the
// mapping cannot disappear while the kernel is consuming its pixels.
class Display final {
public:
    Display();
    ~Display();
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;
    bool open();
    RefreshResult present(const RefreshFrame &frame);
    const Layout &layout() const { return layout_; }
    QString error() const { return error_; }
    unsigned rotation() const { return rotation_; }
    bool swipeAvailable() const { return swipeAvailable_; }
private:
#ifdef __linux__
    int fd = -1;
    unsigned char *mapping = nullptr;
    uint32_t marker = 0;
#endif
    Layout layout_;
    unsigned rotation_ = 0;
    bool swipeAvailable_ = false;
    QString error_;
};
}
