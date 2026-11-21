
#pragma once

#include <cstdint>

namespace uvtf
{

enum class PixelFormat
{
    Unknown,
    YUV420P,
    NV12,
    RGB24,
    BGRA32
};

struct VideoFormat
{
    int width = 0;
    int height = 0;

    PixelFormat pixelFormat = PixelFormat::Unknown;

    int frameRateNumerator = 30;
    int frameRateDenominator = 1;

    bool isValid() const
    {
        return width > 0 &&
               height > 0 &&
               frameRateNumerator > 0 &&
               frameRateDenominator > 0 &&
               pixelFormat != PixelFormat::Unknown;
    }

};

}
