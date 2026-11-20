// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <new>

namespace blaze {
inline bool decodeWebp(LVImageSource *source, LVStreamRef stream, LVImageDecoderCallback *callback,
                       int &width, int &height)
{
    const lvsize_t size = stream->GetSize();
    if (!size || size > 32u * 1024 * 1024)
        return false;
    lUInt8 header[64];
    lvsize_t count = 0;
    stream->SetPos(0);
    if (stream->Read(header, size < sizeof(header) ? size : sizeof(header), &count) != LVERR_OK)
        return false;
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(header, count, &features) != VP8_STATUS_OK ||
        (lUInt64)features.width * features.height > 16u * 1024 * 1024)
        return false;
    width = features.width;
    height = features.height;
    if (!callback)
        return true;

    std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[size]);
    stream->SetPos(0);
    if (!bytes || stream->Read(bytes.get(), size, &count) != LVERR_OK || count != size)
        return false;
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config))
        return false;
    int rasterWidth = width, rasterHeight = height;
    int x = 0, y = 0;
    WebPData data{bytes.get(), size};
    std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(nullptr, WebPDemuxDelete);
    if (features.has_animation) {
        // Only paint the first frame. A playback decoder keeps two full canvases.
        demux.reset(WebPDemux(&data));
        WebPIterator frame;
        if (!demux || !WebPDemuxGetFrame(demux.get(), 1, &frame))
            return false;
        x = frame.x_offset;
        y = frame.y_offset;
        data = frame.fragment; // The bytes remain owned by the input buffer.
        const bool fits = frame.complete && x >= 0 && y >= 0 && frame.width <= width - x
                && frame.height <= height - y;
        WebPDemuxReleaseIterator(&frame);
        if (!fits)
            return false;
    } else {
        int targetWidth, targetHeight;
        if (callback->GetRasterTargetSize(targetWidth, targetHeight)
                && targetWidth > 0 && targetHeight > 0
                && targetWidth <= width && targetHeight <= height) {
            rasterWidth = targetWidth;
            rasterHeight = targetHeight;
            config.options.use_scaling = 1;
            config.options.scaled_width = rasterWidth;
            config.options.scaled_height = rasterHeight;
        }
    }
    const size_t area = size_t(rasterWidth) * rasterHeight;
    std::unique_ptr<lUInt32[]> raster(new (std::nothrow) lUInt32[area]);
    if (!raster)
        return false;
    if (features.has_animation)
        memset(raster.get(), 0, area * sizeof(lUInt32));
    const size_t offset = size_t(y) * rasterWidth + x;
    config.output.colorspace = MODE_BGRA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = reinterpret_cast<uint8_t *>(raster.get() + offset);
    config.output.u.RGBA.stride = rasterWidth * sizeof(lUInt32);
    config.output.u.RGBA.size = (area - offset) * sizeof(lUInt32);
    if (WebPDecode(data.bytes, data.size, &config) != VP8_STATUS_OK)
        return false;
    auto *row = raster.get();
    callback->OnStartRasterDecode(source, rasterWidth, rasterHeight);
    for (int y = 0; y < rasterHeight; ++y, row += rasterWidth) {
        for (int x = 0; x < rasterWidth; ++x)
            row[x] ^= 0xff000000; // Blaze stores inverted alpha.
        callback->OnLineDecoded(source, y, row);
    }
    callback->OnEndDecode(source, false);
    return true;
}
}
